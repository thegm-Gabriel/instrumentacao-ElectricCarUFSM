#include "servicos/servico_ota.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gerenciadores/gerenciador_ota.h"
#include "gerenciadores/gerenciador_ota_satelites.h"
#include "nvs.h"
#include "nucleo/configuracao_ota.h"
#include "nucleo/configuracao_wifi.h"
#include "servicos/servico_wifi.h"
#include "servicos/servico_telemetria.h"
#include "protocolo_telemetria.h"
#include "protocolo_ota_satelites.h"

namespace {
constexpr char ETIQUETA[] = "servico_ota";
constexpr EventBits_t BIT_VERIFICAR = BIT0;
constexpr EventBits_t BIT_AUTORIZAR = BIT1;
constexpr EventBits_t BIT_CANCELAR = BIT2;
constexpr EventBits_t BIT_INTERVALO_ALTERADO = BIT3;
constexpr char ESPACO_NVS_OTA[] = "ota_config";
constexpr char CHAVE_INTERVALO[] = "intervalo_min";

struct FirmwareManifestoOta {
    char versao[32] = {};
    char url_firmware[512] = {};
    char sha256[65] = {};
    uint32_t tamanho_bytes = 0;
};

struct ManifestoOta {
    FirmwareManifestoOta mestre;
    FirmwareManifestoOta equipe;
    FirmwareManifestoOta visitantes;
};

struct RespostaHttp {
    char dados[configuracao::TAMANHO_MAXIMO_MANIFESTO_OTA + 1] = {};
    size_t tamanho = 0;
    bool excedeu_limite = false;
};

struct LiberadorRespostaHttp {
    void operator()(RespostaHttp* resposta) const {
        heap_caps_free(resposta);
    }
};

using RespostaHttpAlocada = std::unique_ptr<RespostaHttp, LiberadorRespostaHttp>;

EventGroupHandle_t eventos_ota = nullptr;
SemaphoreHandle_t mutex_estado = nullptr;
TaskHandle_t tarefa_ota_handle = nullptr;
SituacaoOta situacao{};
ManifestoOta manifesto_disponivel{};
AlvoOta alvo_autorizado = AlvoOta::Nenhum;
bool iniciado = false;

void definir_estado(EstadoServicoOta estado, esp_err_t erro = ESP_OK);
bool interpretar_versao(const char* texto, int partes[3]);

class SessaoWifiOta {
public:
    SessaoWifiOta() = default;
    esp_err_t ativar() {
        definir_estado(EstadoServicoOta::AguardandoRede);
        const esp_err_t erro = servico_wifi_abrir_sessao();
        ativo_ = erro == ESP_OK;
        return erro;
    }
    ~SessaoWifiOta() {
        if (ativo_) {
            servico_wifi_encerrar_sessao();
        }
    }
    SessaoWifiOta(const SessaoWifiOta&) = delete;
    SessaoWifiOta& operator=(const SessaoWifiOta&) = delete;

private:
    bool ativo_ = false;
};

uint32_t carregar_intervalo_verificacao() {
    uint32_t minutos = configuracao::INTERVALO_VERIFICACAO_OTA_PADRAO_MIN;
    nvs_handle_t armazenamento = 0;
    if (nvs_open(ESPACO_NVS_OTA, NVS_READONLY, &armazenamento) == ESP_OK) {
        uint32_t salvo = 0;
        if (nvs_get_u32(armazenamento, CHAVE_INTERVALO, &salvo) == ESP_OK &&
            salvo >= configuracao::INTERVALO_VERIFICACAO_OTA_MINIMO_MIN &&
            salvo <= configuracao::INTERVALO_VERIFICACAO_OTA_MAXIMO_MIN) {
            minutos = salvo;
        }
        nvs_close(armazenamento);
    }
    return minutos;
}

esp_err_t salvar_intervalo_verificacao(uint32_t minutos) {
    nvs_handle_t armazenamento = 0;
    esp_err_t erro = nvs_open(ESPACO_NVS_OTA, NVS_READWRITE, &armazenamento);
    if (erro == ESP_OK) erro = nvs_set_u32(armazenamento, CHAVE_INTERVALO, minutos);
    if (erro == ESP_OK) erro = nvs_commit(armazenamento);
    if (armazenamento != 0) nvs_close(armazenamento);
    return erro;
}

void definir_estado(EstadoServicoOta estado, esp_err_t erro) {
    if (mutex_estado == nullptr) return;
    xSemaphoreTake(mutex_estado, portMAX_DELAY);
    situacao.estado = estado;
    situacao.ultimo_erro = erro;
    if (erro != ESP_OK) situacao.falhas++;
    xSemaphoreGive(mutex_estado);
}

esp_err_t tratar_evento_http(esp_http_client_event_t* evento) {
    if (evento->event_id != HTTP_EVENT_ON_DATA || evento->data == nullptr ||
        evento->data_len <= 0) {
        return ESP_OK;
    }
    const int codigo_http = esp_http_client_get_status_code(evento->client);
    if (codigo_http >= 300 && codigo_http < 400) {
        // Não misturar uma eventual página de redirecionamento do GitHub com
        // o JSON recebido na requisição seguinte.
        return ESP_OK;
    }
    auto* resposta = static_cast<RespostaHttp*>(evento->user_data);
    if (resposta == nullptr) return ESP_ERR_INVALID_ARG;
    const size_t recebido = static_cast<size_t>(evento->data_len);
    if (resposta->tamanho + recebido > configuracao::TAMANHO_MAXIMO_MANIFESTO_OTA) {
        resposta->excedeu_limite = true;
        return ESP_FAIL;
    }
    std::memcpy(&resposta->dados[resposta->tamanho], evento->data, recebido);
    resposta->tamanho += recebido;
    resposta->dados[resposta->tamanho] = '\0';
    return ESP_OK;
}

bool copiar_texto_json(cJSON* raiz, const char* chave, char* destino, size_t capacidade) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(raiz, chave);
    if (!cJSON_IsString(item) || item->valuestring == nullptr || item->valuestring[0] == '\0' ||
        std::strlen(item->valuestring) >= capacidade) {
        return false;
    }
    std::strcpy(destino, item->valuestring);
    return true;
}

bool ler_firmware_manifesto(cJSON* objeto, FirmwareManifestoOta* firmware) {
    if (!cJSON_IsObject(objeto) || firmware == nullptr) return false;
    cJSON* tamanho = cJSON_GetObjectItemCaseSensitive(objeto, "tamanho_bytes");
    const bool valido = copiar_texto_json(objeto, "versao", firmware->versao,
                                          sizeof(firmware->versao)) &&
                        copiar_texto_json(objeto, "url_firmware",
                                          firmware->url_firmware,
                                          sizeof(firmware->url_firmware)) &&
                        copiar_texto_json(objeto, "sha256", firmware->sha256,
                                          sizeof(firmware->sha256)) &&
                        cJSON_IsNumber(tamanho) && tamanho->valuedouble > 0 &&
                        tamanho->valuedouble <= UINT32_MAX;
    if (!valido || std::strncmp(firmware->url_firmware, "https://", 8) != 0 ||
        !gerenciador_ota_sha256_valido(firmware->sha256)) return false;
    firmware->tamanho_bytes = static_cast<uint32_t>(tamanho->valuedouble);
    return true;
}

esp_err_t baixar_manifesto(ManifestoOta* manifesto) {
    if (manifesto == nullptr) return ESP_ERR_INVALID_ARG;
    auto* memoria_resposta = static_cast<RespostaHttp*>(heap_caps_calloc(
        1, sizeof(RespostaHttp), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (memoria_resposta == nullptr) {
        ESP_LOGW(ETIQUETA,
                 "PSRAM sem espaço para o manifesto; tentando a memória interna");
        memoria_resposta = static_cast<RespostaHttp*>(
            heap_caps_calloc(1, sizeof(RespostaHttp), MALLOC_CAP_8BIT));
    }
    RespostaHttpAlocada resposta(memoria_resposta);
    if (!resposta) {
        ESP_LOGE(ETIQUETA, "Memória insuficiente para receber o manifesto OTA");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t configuracao_http{};
    configuracao_http.url = configuracao::ENDERECO_MANIFESTO_OTA;
    configuracao_http.event_handler = tratar_evento_http;
    configuracao_http.user_data = resposta.get();
    configuracao_http.crt_bundle_attach = esp_crt_bundle_attach;
    configuracao_http.timeout_ms = static_cast<int>(configuracao::TEMPO_LIMITE_HTTP_MS);
    configuracao_http.buffer_size_tx = configuracao::TAMANHO_BUFFER_HTTP_TX;
    configuracao_http.max_redirection_count = 8;
    configuracao_http.keep_alive_enable = true;
    configuracao_http.user_agent = "UFSM-Carro-Mestre-Manifesto/1";

    esp_http_client_handle_t cliente = esp_http_client_init(&configuracao_http);
    if (cliente == nullptr) return ESP_ERR_NO_MEM;
    esp_err_t erro = esp_http_client_perform(cliente);
    const int codigo_http = esp_http_client_get_status_code(cliente);
    esp_http_client_cleanup(cliente);

    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Não foi possível baixar o manifesto da atualização: %s",
                 esp_err_to_name(erro));
        return erro;
    }
    if (codigo_http != 200) {
        ESP_LOGE(ETIQUETA,
                 "O servidor respondeu HTTP %d ao buscar o manifesto; confira o release e seus arquivos",
                 codigo_http);
        return ESP_ERR_HTTP_BASE;
    }
    if (resposta->excedeu_limite || resposta->tamanho == 0) {
        ESP_LOGE(ETIQUETA, "Manifesto vazio ou maior que %u bytes",
                 static_cast<unsigned>(configuracao::TAMANHO_MAXIMO_MANIFESTO_OTA));
        return ESP_ERR_INVALID_SIZE;
    }

    cJSON* raiz = cJSON_ParseWithLength(resposta->dados, resposta->tamanho);
    if (raiz == nullptr) return ESP_ERR_INVALID_RESPONSE;
    cJSON* formato = cJSON_GetObjectItemCaseSensitive(raiz, "versao_formato");
    bool estrutura_valida = false;
    if (cJSON_IsNumber(formato) && formato->valueint == 2) {
        cJSON* firmwares = cJSON_GetObjectItemCaseSensitive(raiz, "firmwares");
        estrutura_valida = cJSON_IsObject(firmwares) &&
            ler_firmware_manifesto(cJSON_GetObjectItemCaseSensitive(
                                      firmwares, "mestre-s3"), &manifesto->mestre) &&
            ler_firmware_manifesto(cJSON_GetObjectItemCaseSensitive(
                                      firmwares, "equipe-s3"), &manifesto->equipe) &&
            ler_firmware_manifesto(cJSON_GetObjectItemCaseSensitive(
                                      firmwares, "visitantes-s3"), &manifesto->visitantes) &&
            std::strlen(manifesto->equipe.versao) < OTA_SATELITE_TAMANHO_VERSAO &&
            std::strlen(manifesto->visitantes.versao) < OTA_SATELITE_TAMANHO_VERSAO;
    }
    int partes_versao[3]{};
    estrutura_valida = estrutura_valida &&
        interpretar_versao(manifesto->mestre.versao, partes_versao) &&
        interpretar_versao(manifesto->equipe.versao, partes_versao) &&
        interpretar_versao(manifesto->visitantes.versao, partes_versao);
    cJSON_Delete(raiz);

    if (!estrutura_valida) {
        ESP_LOGE(ETIQUETA,
                 "Manifesto OTA inválido: esperado formato 2 com mestre, equipe e visitantes");
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

bool interpretar_versao(const char* texto, int partes[3]) {
    if (texto == nullptr) return false;
    const char* cursor = texto[0] == 'v' || texto[0] == 'V' ? texto + 1 : texto;
    for (int indice = 0; indice < 3; indice++) {
        if (!std::isdigit(static_cast<unsigned char>(*cursor))) return false;
        char* fim = nullptr;
        long valor = std::strtol(cursor, &fim, 10);
        if (valor < 0 || valor > 65535 || fim == cursor) return false;
        partes[indice] = static_cast<int>(valor);
        if (indice < 2) {
            if (*fim != '.') return false;
            cursor = fim + 1;
        } else if (*fim != '\0' && *fim != '-') {
            return false;
        }
    }
    return true;
}

bool versao_mais_nova(const char* candidata, const char* atual) {
    int nova[3]{};
    int instalada[3]{};
    if (!interpretar_versao(candidata, nova) || !interpretar_versao(atual, instalada)) {
        ESP_LOGE(ETIQUETA, "Versão inválida: disponível='%s' atual='%s'", candidata, atual);
        return false;
    }
    for (int indice = 0; indice < 3; indice++) {
        if (nova[indice] != instalada[indice]) return nova[indice] > instalada[indice];
    }
    return false;
}

SituacaoOtaSatelite avaliar_satelite(const EstatisticasLinkTelemetria& link,
                                     const FirmwareManifestoOta& firmware) {
    SituacaoOtaSatelite resultado{};
    std::strncpy(resultado.versao_disponivel, firmware.versao,
                 sizeof(resultado.versao_disponivel) - 1);
    resultado.progresso.tamanho_total = firmware.tamanho_bytes;
    if (!link.respondeu || link.idade_ultima_confirmacao_ms > 5000) {
        resultado.estado = EstadoOtaSatelite::Indisponivel;
        return resultado;
    }
    std::snprintf(resultado.versao_atual, sizeof(resultado.versao_atual),
                  "%u.%u.%u", static_cast<unsigned>(link.versao_maior),
                  static_cast<unsigned>(link.versao_menor),
                  static_cast<unsigned>(link.versao_correcao));
    if ((link.capacidades & CAPACIDADE_SATELITE_OTA_UART) == 0) {
        resultado.estado = EstadoOtaSatelite::NaoSuportado;
        return resultado;
    }
    resultado.estado = versao_mais_nova(firmware.versao, resultado.versao_atual)
                           ? EstadoOtaSatelite::AguardandoAutorizacao
                           : EstadoOtaSatelite::Atualizado;
    return resultado;
}

esp_err_t verificar_atualizacao() {
    xSemaphoreTake(mutex_estado, portMAX_DELAY);
    situacao.verificacoes++;
    xSemaphoreGive(mutex_estado);
    SessaoWifiOta sessao_wifi;
    esp_err_t erro = sessao_wifi.ativar();
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA,
                 "Não foi possível verificar atualizações porque o Wi-Fi está desconectado");
        definir_estado(EstadoServicoOta::Falha, erro);
        return erro;
    }

    definir_estado(EstadoServicoOta::Verificando);
    ESP_LOGI(ETIQUETA, "Verificando se existe uma nova versão do firmware...");

    ManifestoOta recebido{};
    erro = baixar_manifesto(&recebido);
    if (erro != ESP_OK) {
        definir_estado(EstadoServicoOta::Falha, erro);
        return erro;
    }

    const char* versao_atual = esp_app_get_description()->version;
    const bool mestre_pendente = versao_mais_nova(recebido.mestre.versao, versao_atual);
    const SituacaoOtaSatelite equipe = avaliar_satelite(
        servico_telemetria_obter_estatisticas_link(SateliteTelemetria::Equipe),
        recebido.equipe);
    const SituacaoOtaSatelite visitantes = avaliar_satelite(
        servico_telemetria_obter_estatisticas_link(SateliteTelemetria::Visitantes),
        recebido.visitantes);
    const bool possui_pendente = mestre_pendente ||
        equipe.estado == EstadoOtaSatelite::AguardandoAutorizacao ||
        visitantes.estado == EstadoOtaSatelite::AguardandoAutorizacao;
    xSemaphoreTake(mutex_estado, portMAX_DELAY);
    manifesto_disponivel = recebido;
    situacao.equipe = equipe;
    situacao.visitantes = visitantes;
    if (mestre_pendente) {
        std::strcpy(situacao.versao_disponivel, recebido.mestre.versao);
    } else {
        situacao.versao_disponivel[0] = '\0';
    }
    situacao.estado = possui_pendente ? EstadoServicoOta::AguardandoAutorizacao
                                      : EstadoServicoOta::Atualizado;
    situacao.alvo_ativo = AlvoOta::Nenhum;
    situacao.ultimo_erro = ESP_OK;
    xSemaphoreGive(mutex_estado);
    ESP_LOGI(ETIQUETA, "Resultado OTA: mestre=%s | equipe=%s | visitantes=%s",
             mestre_pendente ? "atualização disponível" : "atualizado",
             servico_ota_nome_estado_satelite(equipe.estado),
             servico_ota_nome_estado_satelite(visitantes.estado));
    if (possui_pendente) {
        ESP_LOGW(ETIQUETA,
                 "Há firmware novo aguardando autorização no menu de atualização OTA");
    } else {
        ESP_LOGI(ETIQUETA, "Todos os alvos acessíveis já usam as versões mais recentes");
    }
    return ESP_OK;
}

void calcular_progresso(ProgressoTransferenciaOta* progresso, uint32_t bytes,
                        uint32_t total, uint32_t decorrido_ms) {
    progresso->bytes_transferidos = bytes;
    progresso->tamanho_total = total;
    progresso->tempo_decorrido_ms = decorrido_ms;
    progresso->percentual_decimos = total == 0
        ? 0 : (bytes >= total ? 1000u : static_cast<uint32_t>(
              (static_cast<uint64_t>(bytes) * 1000u) / total));
    progresso->taxa_bytes_por_segundo = decorrido_ms == 0
                                      ? 0
                                      : static_cast<uint32_t>(
                                            (static_cast<uint64_t>(bytes) * 1000u) /
                                            decorrido_ms);
    const uint32_t restantes = bytes >= total ? 0 : total - bytes;
    progresso->tempo_restante_segundos = progresso->taxa_bytes_por_segundo == 0
        ? 0 : (restantes + progresso->taxa_bytes_por_segundo - 1) /
              progresso->taxa_bytes_por_segundo;
}

void atualizar_progresso_satelite(uint32_t bytes, uint32_t total,
                                  uint32_t decorrido_ms) {
    xSemaphoreTake(mutex_estado, portMAX_DELAY);
    SituacaoOtaSatelite* alvo = situacao.alvo_ativo == AlvoOta::Equipe
                                    ? &situacao.equipe : &situacao.visitantes;
    const uint32_t percentual_anterior = alvo->progresso.percentual_decimos;
    calcular_progresso(&alvo->progresso, bytes, total, decorrido_ms);
    const uint32_t percentual = alvo->progresso.percentual_decimos;
    const uint32_t taxa = alvo->progresso.taxa_bytes_por_segundo;
    const AlvoOta nome_alvo = situacao.alvo_ativo;
    xSemaphoreGive(mutex_estado);
    if (percentual / 100u != percentual_anterior / 100u || percentual == 1000u) {
        ESP_LOGI(ETIQUETA, "OTA %s: %lu.%lu%% (%lu/%lu bytes, %.2f MB/s)",
                 servico_ota_nome_alvo(nome_alvo),
                 static_cast<unsigned long>(percentual / 10u),
                 static_cast<unsigned long>(percentual % 10u),
                 static_cast<unsigned long>(bytes), static_cast<unsigned long>(total),
                 taxa / (1024.0 * 1024.0));
    }
}

void atualizar_progresso_mestre(uint32_t bytes, uint32_t total,
                                uint32_t decorrido_ms) {
    xSemaphoreTake(mutex_estado, portMAX_DELAY);
    calcular_progresso(&situacao.progresso, bytes, total, decorrido_ms);
    xSemaphoreGive(mutex_estado);
}

void executar_atualizacao_mestre(const FirmwareManifestoOta& firmware) {
    ESP_LOGW(ETIQUETA,
             "Atualização do mestre para %s autorizada. Iniciando download seguro",
             firmware.versao);
    ConfiguracaoOta configuracao_gerenciador{};
    configuracao_gerenciador.endereco_https = firmware.url_firmware;
    configuracao_gerenciador.versao_esperada = firmware.versao;
    configuracao_gerenciador.sha256_esperado = firmware.sha256;
    configuracao_gerenciador.tempo_limite_ms = configuracao::TEMPO_LIMITE_HTTP_MS;
    configuracao_gerenciador.tamanho_esperado = firmware.tamanho_bytes;
    configuracao_gerenciador.tamanho_buffer_http_tx =
        configuracao::TAMANHO_BUFFER_HTTP_TX;
    configuracao_gerenciador.observador_progresso = atualizar_progresso_mestre;

    esp_err_t erro = gerenciador_ota_iniciar(configuracao_gerenciador);
    if (erro == ESP_OK) erro = gerenciador_ota_executar();
    if (erro != ESP_OK) {
        definir_estado(EstadoServicoOta::Falha, erro);
        ESP_LOGE(ETIQUETA,
                 "A atualização do mestre falhou: %s. O firmware atual continua ativo",
                 esp_err_to_name(erro));
        return;
    }
    definir_estado(EstadoServicoOta::Aplicado);
    ESP_LOGW(ETIQUETA, "Firmware do mestre validado. Reinício em 3 segundos");
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
}

void executar_atualizacao_satelite(AlvoOta alvo,
                                   const FirmwareManifestoOta& firmware) {
    xSemaphoreTake(mutex_estado, portMAX_DELAY);
    SituacaoOtaSatelite* estado = alvo == AlvoOta::Equipe
                                      ? &situacao.equipe : &situacao.visitantes;
    estado->estado = EstadoOtaSatelite::Transferindo;
    estado->progresso = {};
    estado->progresso.tamanho_total = firmware.tamanho_bytes;
    estado->ultimo_erro = ESP_OK;
    situacao.estado = EstadoServicoOta::Baixando;
    situacao.alvo_ativo = alvo;
    situacao.progresso = {};
    xSemaphoreGive(mutex_estado);

    ConfiguracaoTransferenciaOtaSatelite transferencia{};
    transferencia.destino = alvo == AlvoOta::Equipe
                                ? DestinoUart::Equipe : DestinoUart::Visitantes;
    transferencia.endereco_https = firmware.url_firmware;
    transferencia.versao = firmware.versao;
    transferencia.sha256 = firmware.sha256;
    transferencia.tamanho_bytes = firmware.tamanho_bytes;
    transferencia.tempo_limite_http_ms = configuracao::TEMPO_LIMITE_HTTP_MS;
    servico_telemetria_definir_ota_satelite_ativa(true);
    const esp_err_t erro = gerenciador_ota_satelites_executar(
        transferencia, atualizar_progresso_satelite);
    servico_telemetria_definir_ota_satelite_ativa(false);

    xSemaphoreTake(mutex_estado, portMAX_DELAY);
    estado = alvo == AlvoOta::Equipe ? &situacao.equipe : &situacao.visitantes;
    estado->ultimo_erro = erro;
    estado->estado = erro == ESP_OK ? EstadoOtaSatelite::Reiniciando
                                    : EstadoOtaSatelite::Falha;
    situacao.estado = erro == ESP_OK ? EstadoServicoOta::Aplicado
                                     : EstadoServicoOta::Falha;
    situacao.ultimo_erro = erro;
    if (erro != ESP_OK) situacao.falhas++;
    xSemaphoreGive(mutex_estado);
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "OTA do satélite %s falhou: %s; firmware anterior preservado",
                 servico_ota_nome_alvo(alvo), esp_err_to_name(erro));
        return;
    }
    ESP_LOGW(ETIQUETA, "OTA do satélite %s concluída; ele está reiniciando",
             servico_ota_nome_alvo(alvo));
    vTaskDelay(pdMS_TO_TICKS(3000));
    xSemaphoreTake(mutex_estado, portMAX_DELAY);
    estado = alvo == AlvoOta::Equipe ? &situacao.equipe : &situacao.visitantes;
    estado->estado = EstadoOtaSatelite::Concluido;
    const bool ainda_pendente = situacao.versao_disponivel[0] != '\0' ||
        situacao.equipe.estado == EstadoOtaSatelite::AguardandoAutorizacao ||
        situacao.visitantes.estado == EstadoOtaSatelite::AguardandoAutorizacao;
    situacao.estado = ainda_pendente ? EstadoServicoOta::AguardandoAutorizacao
                                     : EstadoServicoOta::Atualizado;
    situacao.alvo_ativo = AlvoOta::Nenhum;
    xSemaphoreGive(mutex_estado);
}

void executar_atualizacao_autorizada() {
    ManifestoOta manifesto{};
    AlvoOta alvo = AlvoOta::Nenhum;
    xSemaphoreTake(mutex_estado, portMAX_DELAY);
    alvo = alvo_autorizado;
    if (situacao.estado != EstadoServicoOta::AguardandoAutorizacao ||
        alvo == AlvoOta::Nenhum) {
        xSemaphoreGive(mutex_estado);
        ESP_LOGW(ETIQUETA,
                 "A instalação não foi iniciada porque não existe atualização pendente");
        return;
    }
    manifesto = manifesto_disponivel;
    alvo_autorizado = AlvoOta::Nenhum;
    xSemaphoreGive(mutex_estado);

    SessaoWifiOta sessao_wifi;
    esp_err_t erro = sessao_wifi.ativar();
    if (erro != ESP_OK) {
        definir_estado(EstadoServicoOta::Falha, erro);
        ESP_LOGE(ETIQUETA, "A atualização não começou porque o Wi-Fi está indisponível");
        return;
    }
    definir_estado(EstadoServicoOta::Baixando);
    xSemaphoreTake(mutex_estado, portMAX_DELAY);
    situacao.alvo_ativo = alvo;
    situacao.progresso = {};
    if (alvo == AlvoOta::Mestre) {
        situacao.progresso.tamanho_total = manifesto.mestre.tamanho_bytes;
    }
    xSemaphoreGive(mutex_estado);
    if (alvo == AlvoOta::Mestre) {
        executar_atualizacao_mestre(manifesto.mestre);
    } else if (alvo == AlvoOta::Equipe) {
        executar_atualizacao_satelite(alvo, manifesto.equipe);
    } else {
        executar_atualizacao_satelite(alvo, manifesto.visitantes);
    }
}

void cancelar_atualizacao() {
    xSemaphoreTake(mutex_estado, portMAX_DELAY);
    if (situacao.estado == EstadoServicoOta::AguardandoAutorizacao) {
        manifesto_disponivel = {};
        alvo_autorizado = AlvoOta::Nenhum;
        situacao.versao_disponivel[0] = '\0';
        if (situacao.equipe.estado == EstadoOtaSatelite::AguardandoAutorizacao)
            situacao.equipe.estado = EstadoOtaSatelite::Cancelado;
        if (situacao.visitantes.estado == EstadoOtaSatelite::AguardandoAutorizacao)
            situacao.visitantes.estado = EstadoOtaSatelite::Cancelado;
        situacao.estado = EstadoServicoOta::Cancelado;
        situacao.ultimo_erro = ESP_OK;
        ESP_LOGW(ETIQUETA,
                 "Atualização cancelada. Nenhuma alteração foi feita no firmware em execução");
    }
    xSemaphoreGive(mutex_estado);
}

void tarefa_ota(void*) {
    bool verificar = true;
    while (true) {
        if (verificar) verificar_atualizacao();
        xSemaphoreTake(mutex_estado, portMAX_DELAY);
        const uint32_t intervalo_minutos = situacao.intervalo_verificacao_minutos;
        xSemaphoreGive(mutex_estado);
        const TickType_t espera = pdMS_TO_TICKS(intervalo_minutos * 60UL * 1000UL);
        EventBits_t eventos = xEventGroupWaitBits(
            eventos_ota, BIT_VERIFICAR | BIT_AUTORIZAR | BIT_CANCELAR |
                            BIT_INTERVALO_ALTERADO,
            pdTRUE, pdFALSE, espera);
        if ((eventos & BIT_CANCELAR) != 0) cancelar_atualizacao();
        if ((eventos & BIT_AUTORIZAR) != 0) executar_atualizacao_autorizada();
        verificar = eventos == 0 || (eventos & BIT_VERIFICAR) != 0;
    }
}

}  // namespace

esp_err_t servico_ota_iniciar() {
    if (!configuracao::HABILITAR_OTA) return ESP_ERR_NOT_SUPPORTED;
    if (iniciado) return ESP_OK;
    if (!servico_wifi_obter_resumo().inicializado) {
        ESP_LOGE(ETIQUETA, "OTA requer que o serviço Wi-Fi seja iniciado primeiro");
        return ESP_ERR_INVALID_STATE;
    }
    if (mutex_estado == nullptr) mutex_estado = xSemaphoreCreateMutex();
    if (eventos_ota == nullptr) eventos_ota = xEventGroupCreate();
    if (mutex_estado == nullptr || eventos_ota == nullptr) {
        if (eventos_ota != nullptr) {
            vEventGroupDelete(eventos_ota);
            eventos_ota = nullptr;
        }
        if (mutex_estado != nullptr) {
            vSemaphoreDelete(mutex_estado);
            mutex_estado = nullptr;
        }
        return ESP_ERR_NO_MEM;
    }

    xEventGroupClearBits(eventos_ota, BIT_VERIFICAR | BIT_AUTORIZAR |
                                      BIT_CANCELAR | BIT_INTERVALO_ALTERADO);
    xSemaphoreTake(mutex_estado, portMAX_DELAY);
    situacao = {};
    manifesto_disponivel = {};
    alvo_autorizado = AlvoOta::Nenhum;
    situacao.estado = EstadoServicoOta::AguardandoRede;
    std::strncpy(situacao.versao_atual, esp_app_get_description()->version,
                 sizeof(situacao.versao_atual) - 1);
    situacao.intervalo_verificacao_minutos = carregar_intervalo_verificacao();
    xSemaphoreGive(mutex_estado);

    if (xTaskCreate(tarefa_ota, "servico_ota", 14336, nullptr, 4,
                    &tarefa_ota_handle) != pdPASS) {
        tarefa_ota_handle = nullptr;
        definir_estado(EstadoServicoOta::Falha, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    iniciado = true;
    ESP_LOGI(ETIQUETA,
             "Serviço OTA pronto. Versão instalada: %s. Verificação inicial e depois a cada %lu minuto(s)",
             situacao.versao_atual,
             static_cast<unsigned long>(situacao.intervalo_verificacao_minutos));
    return ESP_OK;
}

esp_err_t servico_ota_confirmar_firmware_em_execucao() {
    return gerenciador_ota_confirmar_firmware_em_execucao();
}

esp_err_t servico_ota_rejeitar_firmware_em_execucao() {
    return gerenciador_ota_rejeitar_firmware_em_execucao();
}

esp_err_t servico_ota_solicitar_verificacao() {
    if (!iniciado || eventos_ota == nullptr) return ESP_ERR_INVALID_STATE;
    xEventGroupSetBits(eventos_ota, BIT_VERIFICAR);
    return ESP_OK;
}

esp_err_t servico_ota_autorizar_atualizacao() {
    if (!iniciado || eventos_ota == nullptr) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(mutex_estado, portMAX_DELAY);
    const bool aguardando = situacao.estado == EstadoServicoOta::AguardandoAutorizacao &&
                            situacao.versao_disponivel[0] != '\0' &&
                            alvo_autorizado == AlvoOta::Nenhum;
    if (aguardando) alvo_autorizado = AlvoOta::Mestre;
    xSemaphoreGive(mutex_estado);
    if (!aguardando) {
        ESP_LOGW(ETIQUETA,
                 "Não há atualização pendente para autorizar. Verifique primeiro se existe uma nova versão");
        return ESP_ERR_INVALID_STATE;
    }
    xEventGroupSetBits(eventos_ota, BIT_AUTORIZAR);
    return ESP_OK;
}

esp_err_t servico_ota_autorizar_atualizacao_satelite(AlvoOta alvo) {
    if (!iniciado || eventos_ota == nullptr ||
        (alvo != AlvoOta::Equipe && alvo != AlvoOta::Visitantes)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(mutex_estado, portMAX_DELAY);
    const SituacaoOtaSatelite& satelite = alvo == AlvoOta::Equipe
                                              ? situacao.equipe : situacao.visitantes;
    const bool aguardando = situacao.estado == EstadoServicoOta::AguardandoAutorizacao &&
                            satelite.estado == EstadoOtaSatelite::AguardandoAutorizacao &&
                            alvo_autorizado == AlvoOta::Nenhum;
    if (aguardando) alvo_autorizado = alvo;
    xSemaphoreGive(mutex_estado);
    if (!aguardando) {
        ESP_LOGW(ETIQUETA, "Não há atualização pendente e compatível para %s",
                 servico_ota_nome_alvo(alvo));
        return ESP_ERR_INVALID_STATE;
    }
    xEventGroupSetBits(eventos_ota, BIT_AUTORIZAR);
    return ESP_OK;
}

esp_err_t servico_ota_cancelar_atualizacao() {
    if (!iniciado || eventos_ota == nullptr) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(mutex_estado, portMAX_DELAY);
    const bool aguardando = situacao.estado == EstadoServicoOta::AguardandoAutorizacao;
    xSemaphoreGive(mutex_estado);
    if (!aguardando) return ESP_ERR_INVALID_STATE;
    xEventGroupSetBits(eventos_ota, BIT_CANCELAR);
    return ESP_OK;
}

esp_err_t servico_ota_definir_intervalo_verificacao(uint32_t minutos) {
    if (!iniciado || mutex_estado == nullptr || eventos_ota == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (minutos < configuracao::INTERVALO_VERIFICACAO_OTA_MINIMO_MIN ||
        minutos > configuracao::INTERVALO_VERIFICACAO_OTA_MAXIMO_MIN) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t erro = salvar_intervalo_verificacao(minutos);
    if (erro != ESP_OK) return erro;
    xSemaphoreTake(mutex_estado, portMAX_DELAY);
    situacao.intervalo_verificacao_minutos = minutos;
    xSemaphoreGive(mutex_estado);
    xEventGroupSetBits(eventos_ota, BIT_INTERVALO_ALTERADO);
    ESP_LOGI(ETIQUETA, "Intervalo das verificações OTA alterado para %lu minuto(s)",
             static_cast<unsigned long>(minutos));
    return ESP_OK;
}

SituacaoOta servico_ota_obter_situacao() {
    if (mutex_estado == nullptr) {
        SituacaoOta inicial{};
        std::strncpy(inicial.versao_atual, esp_app_get_description()->version,
                     sizeof(inicial.versao_atual) - 1);
        return inicial;
    }
    xSemaphoreTake(mutex_estado, portMAX_DELAY);
    const SituacaoOta copia = situacao;
    xSemaphoreGive(mutex_estado);
    return copia;
}

const char* servico_ota_nome_estado(EstadoServicoOta estado) {
    switch (estado) {
        case EstadoServicoOta::Desabilitado: return "desabilitado";
        case EstadoServicoOta::Inicializando: return "inicializando";
        case EstadoServicoOta::AguardandoRede: return "aguardando rede";
        case EstadoServicoOta::Verificando: return "verificando";
        case EstadoServicoOta::Atualizado: return "atualizado";
        case EstadoServicoOta::AguardandoAutorizacao: return "aguardando autorização";
        case EstadoServicoOta::Baixando: return "baixando";
        case EstadoServicoOta::Aplicado: return "aplicado";
        case EstadoServicoOta::Cancelado: return "cancelado";
        case EstadoServicoOta::Falha: return "falha";
    }
    return "desconhecido";
}

const char* servico_ota_nome_estado_satelite(EstadoOtaSatelite estado) {
    switch (estado) {
        case EstadoOtaSatelite::Desconhecido: return "desconhecido";
        case EstadoOtaSatelite::Indisponivel: return "sem comunicação";
        case EstadoOtaSatelite::NaoSuportado: return "OTA ainda não suportada";
        case EstadoOtaSatelite::Atualizado: return "atualizado";
        case EstadoOtaSatelite::AguardandoAutorizacao: return "aguardando autorização";
        case EstadoOtaSatelite::Transferindo: return "transferindo";
        case EstadoOtaSatelite::Reiniciando: return "reiniciando";
        case EstadoOtaSatelite::Concluido: return "concluído";
        case EstadoOtaSatelite::Cancelado: return "cancelado";
        case EstadoOtaSatelite::Falha: return "falha";
    }
    return "desconhecido";
}

const char* servico_ota_nome_alvo(AlvoOta alvo) {
    switch (alvo) {
        case AlvoOta::Mestre: return "mestre";
        case AlvoOta::Equipe: return "equipe";
        case AlvoOta::Visitantes: return "visitantes";
        case AlvoOta::Nenhum: return "nenhum";
    }
    return "desconhecido";
}
