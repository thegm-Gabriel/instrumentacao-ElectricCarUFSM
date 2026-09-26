#include "servicos/servico_ota.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gerenciadores/gerenciador_ota.h"
#include "gerenciadores/gerenciador_wifi.h"
#include "nucleo/configuracao_ota.h"

namespace {
constexpr char ETIQUETA[] = "servico_ota";
constexpr EventBits_t BIT_VERIFICAR = BIT0;
constexpr EventBits_t BIT_AUTORIZAR = BIT1;
constexpr EventBits_t BIT_CANCELAR = BIT2;

struct ManifestoOta {
    char versao[32] = {};
    char url_firmware[512] = {};
    char sha256[65] = {};
};

struct RespostaHttp {
    char dados[configuracao::TAMANHO_MAXIMO_MANIFESTO_OTA + 1] = {};
    size_t tamanho = 0;
    bool excedeu_limite = false;
};

EventGroupHandle_t eventos_ota = nullptr;
SemaphoreHandle_t mutex_estado = nullptr;
SituacaoOta situacao{};
ManifestoOta manifesto_disponivel{};
bool iniciado = false;

void definir_estado(EstadoServicoOta estado, esp_err_t erro = ESP_OK) {
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

bool sha256_valido(const char* texto) {
    if (texto == nullptr || std::strlen(texto) != 64) return false;
    for (size_t indice = 0; indice < 64; indice++) {
        if (!std::isxdigit(static_cast<unsigned char>(texto[indice]))) return false;
    }
    return true;
}

esp_err_t baixar_manifesto(ManifestoOta* manifesto) {
    if (manifesto == nullptr) return ESP_ERR_INVALID_ARG;
    RespostaHttp resposta{};

    esp_http_client_config_t configuracao_http{};
    configuracao_http.url = configuracao::ENDERECO_MANIFESTO_OTA;
    configuracao_http.event_handler = tratar_evento_http;
    configuracao_http.user_data = &resposta;
    configuracao_http.crt_bundle_attach = esp_crt_bundle_attach;
    configuracao_http.timeout_ms = static_cast<int>(configuracao::TEMPO_LIMITE_HTTP_MS);
    configuracao_http.max_redirection_count = 8;
    configuracao_http.keep_alive_enable = true;
    configuracao_http.user_agent = "UFSM-Carro-Mestre-Manifesto/1";

    esp_http_client_handle_t cliente = esp_http_client_init(&configuracao_http);
    if (cliente == nullptr) return ESP_ERR_NO_MEM;
    esp_err_t erro = esp_http_client_perform(cliente);
    const int codigo_http = esp_http_client_get_status_code(cliente);
    esp_http_client_cleanup(cliente);

    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Falha ao baixar manifesto: %s", esp_err_to_name(erro));
        return erro;
    }
    if (codigo_http != 200) {
        ESP_LOGE(ETIQUETA, "GitHub respondeu HTTP %d ao manifesto", codigo_http);
        return ESP_ERR_HTTP_BASE;
    }
    if (resposta.excedeu_limite || resposta.tamanho == 0) {
        ESP_LOGE(ETIQUETA, "Manifesto vazio ou maior que %u bytes",
                 static_cast<unsigned>(configuracao::TAMANHO_MAXIMO_MANIFESTO_OTA));
        return ESP_ERR_INVALID_SIZE;
    }

    cJSON* raiz = cJSON_ParseWithLength(resposta.dados, resposta.tamanho);
    if (raiz == nullptr) return ESP_ERR_INVALID_RESPONSE;
    cJSON* formato = cJSON_GetObjectItemCaseSensitive(raiz, "versao_formato");
    cJSON* alvo = cJSON_GetObjectItemCaseSensitive(raiz, "alvo");
    const bool estrutura_valida = cJSON_IsNumber(formato) && formato->valueint == 1 &&
                                  cJSON_IsString(alvo) &&
                                  std::strcmp(alvo->valuestring, "mestre-s3") == 0 &&
                                  copiar_texto_json(raiz, "versao", manifesto->versao,
                                                    sizeof(manifesto->versao)) &&
                                  copiar_texto_json(raiz, "url_firmware",
                                                    manifesto->url_firmware,
                                                    sizeof(manifesto->url_firmware)) &&
                                  copiar_texto_json(raiz, "sha256", manifesto->sha256,
                                                    sizeof(manifesto->sha256));
    cJSON_Delete(raiz);

    if (!estrutura_valida || std::strncmp(manifesto->url_firmware, "https://", 8) != 0 ||
        !sha256_valido(manifesto->sha256)) {
        ESP_LOGE(ETIQUETA, "Manifesto OTA inválido ou incompleto");
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

esp_err_t garantir_rede() {
    if (gerenciador_wifi_esta_conectado()) return ESP_OK;
    definir_estado(EstadoServicoOta::AguardandoRede);
    gerenciador_wifi_reconectar();
    return gerenciador_wifi_aguardar_conexao(
        pdMS_TO_TICKS(configuracao::TEMPO_LIMITE_CONEXAO_WIFI_MS));
}

esp_err_t verificar_atualizacao() {
    esp_err_t erro = garantir_rede();
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Sem rede para verificar atualização");
        definir_estado(EstadoServicoOta::Falha, erro);
        return erro;
    }

    definir_estado(EstadoServicoOta::Verificando);
    xSemaphoreTake(mutex_estado, portMAX_DELAY);
    situacao.verificacoes++;
    xSemaphoreGive(mutex_estado);
    ESP_LOGI(ETIQUETA, "Consultando %s", configuracao::ENDERECO_MANIFESTO_OTA);

    ManifestoOta recebido{};
    erro = baixar_manifesto(&recebido);
    if (erro != ESP_OK) {
        definir_estado(EstadoServicoOta::Falha, erro);
        return erro;
    }

    const char* versao_atual = esp_app_get_description()->version;
    if (!versao_mais_nova(recebido.versao, versao_atual)) {
        xSemaphoreTake(mutex_estado, portMAX_DELAY);
        manifesto_disponivel = {};
        situacao.versao_disponivel[0] = '\0';
        situacao.estado = EstadoServicoOta::Atualizado;
        situacao.ultimo_erro = ESP_OK;
        xSemaphoreGive(mutex_estado);
        ESP_LOGI(ETIQUETA, "Firmware %s já está atualizado", versao_atual);
        return ESP_OK;
    }

    xSemaphoreTake(mutex_estado, portMAX_DELAY);
    manifesto_disponivel = recebido;
    std::strcpy(situacao.versao_disponivel, recebido.versao);
    situacao.estado = EstadoServicoOta::AguardandoAutorizacao;
    situacao.ultimo_erro = ESP_OK;
    xSemaphoreGive(mutex_estado);
    ESP_LOGW(ETIQUETA, "Atualização %s disponível. Digite 'ota autorizar' para instalar",
             recebido.versao);
    ESP_LOGW(ETIQUETA, "Use 'ota cancelar' para recusar esta solicitação");
    return ESP_OK;
}

void executar_atualizacao_autorizada() {
    ManifestoOta manifesto{};
    xSemaphoreTake(mutex_estado, portMAX_DELAY);
    if (situacao.estado != EstadoServicoOta::AguardandoAutorizacao) {
        xSemaphoreGive(mutex_estado);
        ESP_LOGW(ETIQUETA, "Não há atualização aguardando autorização");
        return;
    }
    manifesto = manifesto_disponivel;
    situacao.estado = EstadoServicoOta::Baixando;
    xSemaphoreGive(mutex_estado);

    ESP_LOGW(ETIQUETA, "Atualização %s autorizada pelo operador", manifesto.versao);
    ConfiguracaoOta configuracao_gerenciador{};
    configuracao_gerenciador.endereco_https = manifesto.url_firmware;
    configuracao_gerenciador.versao_esperada = manifesto.versao;
    configuracao_gerenciador.sha256_esperado = manifesto.sha256;
    configuracao_gerenciador.tempo_limite_ms = configuracao::TEMPO_LIMITE_HTTP_MS;

    esp_err_t erro = gerenciador_ota_iniciar(configuracao_gerenciador);
    if (erro == ESP_OK) erro = gerenciador_ota_executar();
    if (erro != ESP_OK) {
        definir_estado(EstadoServicoOta::Falha, erro);
        ESP_LOGE(ETIQUETA, "Atualização falhou: %s", esp_err_to_name(erro));
        return;
    }

    definir_estado(EstadoServicoOta::Aplicado);
    ESP_LOGW(ETIQUETA, "Atualização concluída; reiniciando em 3 segundos");
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
}

void cancelar_atualizacao() {
    xSemaphoreTake(mutex_estado, portMAX_DELAY);
    if (situacao.estado == EstadoServicoOta::AguardandoAutorizacao) {
        manifesto_disponivel = {};
        situacao.versao_disponivel[0] = '\0';
        situacao.estado = EstadoServicoOta::Cancelado;
        situacao.ultimo_erro = ESP_OK;
        ESP_LOGW(ETIQUETA, "Atualização cancelada pelo operador");
    }
    xSemaphoreGive(mutex_estado);
}

void tarefa_ota(void*) {
    bool verificar = true;
    while (true) {
        if (verificar) verificar_atualizacao();
        EventBits_t eventos = xEventGroupWaitBits(
            eventos_ota, BIT_VERIFICAR | BIT_AUTORIZAR | BIT_CANCELAR,
            pdTRUE, pdFALSE, pdMS_TO_TICKS(configuracao::INTERVALO_VERIFICACAO_OTA_MS));
        if ((eventos & BIT_CANCELAR) != 0) cancelar_atualizacao();
        if ((eventos & BIT_AUTORIZAR) != 0) executar_atualizacao_autorizada();
        verificar = eventos == 0 || (eventos & BIT_VERIFICAR) != 0;
    }
}

void mostrar_comandos() {
    ESP_LOGI(ETIQUETA, "Comandos OTA: 'ota estado', 'ota verificar', 'ota autorizar', 'ota cancelar'");
}

void tarefa_terminal_ota(void*) {
    char linha[96];
    mostrar_comandos();
    while (true) {
        if (std::fgets(linha, sizeof(linha), stdin) == nullptr) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        linha[std::strcspn(linha, "\r\n")] = '\0';
        for (char* caractere = linha; *caractere != '\0'; caractere++) {
            *caractere = static_cast<char>(std::tolower(static_cast<unsigned char>(*caractere)));
        }

        if (std::strcmp(linha, "ota verificar") == 0) {
            servico_ota_solicitar_verificacao();
        } else if (std::strcmp(linha, "ota autorizar") == 0) {
            servico_ota_autorizar_atualizacao();
        } else if (std::strcmp(linha, "ota cancelar") == 0) {
            servico_ota_cancelar_atualizacao();
        } else if (std::strcmp(linha, "ota estado") == 0) {
            const SituacaoOta copia = servico_ota_obter_situacao();
            ESP_LOGI(ETIQUETA, "Estado=%s atual=%s disponível=%s verificações=%lu falhas=%lu último=%s",
                     servico_ota_nome_estado(copia.estado), copia.versao_atual,
                     copia.versao_disponivel[0] == '\0' ? "-" : copia.versao_disponivel,
                     static_cast<unsigned long>(copia.verificacoes),
                     static_cast<unsigned long>(copia.falhas),
                     esp_err_to_name(copia.ultimo_erro));
        } else if (std::strcmp(linha, "ota ajuda") == 0) {
            mostrar_comandos();
        } else if (linha[0] != '\0') {
            ESP_LOGW(ETIQUETA, "Comando desconhecido. Digite 'ota ajuda'");
        }
    }
}
}  // namespace

esp_err_t servico_ota_iniciar() {
    if (!configuracao::HABILITAR_OTA) return ESP_ERR_NOT_SUPPORTED;
    if (iniciado) return ESP_OK;
    mutex_estado = xSemaphoreCreateMutex();
    eventos_ota = xEventGroupCreate();
    if (mutex_estado == nullptr || eventos_ota == nullptr) return ESP_ERR_NO_MEM;

    definir_estado(EstadoServicoOta::Inicializando);
    std::strncpy(situacao.versao_atual, esp_app_get_description()->version,
                 sizeof(situacao.versao_atual) - 1);

    esp_err_t erro = gerenciador_wifi_iniciar(configuracao::WIFI_OTA_SSID,
                                               configuracao::WIFI_OTA_SENHA);
    if (erro != ESP_OK) {
        definir_estado(EstadoServicoOta::Falha, erro);
        return erro;
    }
    definir_estado(EstadoServicoOta::AguardandoRede);

    iniciado = true;
    TaskHandle_t tarefa_terminal_criada = nullptr;
    if (xTaskCreate(tarefa_terminal_ota, "terminal_ota", 4096, nullptr, 3,
                    &tarefa_terminal_criada) != pdPASS) {
        iniciado = false;
        definir_estado(EstadoServicoOta::Falha, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(tarefa_ota, "servico_ota", 10240, nullptr, 4, nullptr) != pdPASS) {
        vTaskDelete(tarefa_terminal_criada);
        iniciado = false;
        definir_estado(EstadoServicoOta::Falha, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(ETIQUETA, "Serviço OTA iniciado; versão atual %s", situacao.versao_atual);
    return ESP_OK;
}

esp_err_t servico_ota_solicitar_verificacao() {
    if (!iniciado || eventos_ota == nullptr) return ESP_ERR_INVALID_STATE;
    xEventGroupSetBits(eventos_ota, BIT_VERIFICAR);
    return ESP_OK;
}

esp_err_t servico_ota_autorizar_atualizacao() {
    if (!iniciado || eventos_ota == nullptr) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(mutex_estado, portMAX_DELAY);
    const bool aguardando = situacao.estado == EstadoServicoOta::AguardandoAutorizacao;
    xSemaphoreGive(mutex_estado);
    if (!aguardando) {
        ESP_LOGW(ETIQUETA, "Autorização ignorada: nenhuma atualização está pendente");
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
