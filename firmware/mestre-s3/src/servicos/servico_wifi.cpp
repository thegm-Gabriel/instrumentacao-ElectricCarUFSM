#include "servicos/servico_wifi.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gerenciadores/gerenciador_wifi.h"
#include "nvs.h"

namespace {
constexpr char ETIQUETA[] = "servico_wifi";
constexpr char ESPACO_NVS[] = "wifi_perfis";
constexpr uint8_t VERSAO_ARMAZENAMENTO = 1;
constexpr TickType_t TEMPO_DESLIGAMENTO = pdMS_TO_TICKS(3000);
static_assert(configuracao::MAXIMO_REDES_WIFI_USUARIO <= 10,
              "As chaves NVS reservam um dígito para o índice da rede");

struct PerfilWifi {
    char ssid[33] = {};
    char senha[64] = {};
};

struct ConfiguracaoPerfis {
    bool principal_alterada = false;
    bool automatico = false;
    PerfilWifi principal{};
    uint8_t quantidade = 0;
    std::array<PerfilWifi, configuracao::MAXIMO_REDES_WIFI_USUARIO> redes{};
    PoliticaConexaoWifi politica{};
};

struct EstadoServicoWifi {
    bool iniciado = false;
    bool sessao_em_andamento = false;
    bool aguardando_autorizacao_secundaria = false;
    uint32_t tentativas_ultima_sessao = 0;
};

ConfiguracaoPerfis perfis{};
PerfilWifi principal_firmware_cache{};
portMUX_TYPE trava_perfis = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE trava_estado_servico = portMUX_INITIALIZER_UNLOCKED;
EstadoServicoWifi estado_servico{};
SemaphoreHandle_t mutex_alteracoes = nullptr;
bool existe_perfil_autorizado_uma_vez = false;
PerfilWifi perfil_autorizado_uma_vez{};

class BloqueioAlteracoes {
public:
    explicit BloqueioAlteracoes(TickType_t tempo_limite = portMAX_DELAY)
        : adquirido_(mutex_alteracoes != nullptr &&
                     xSemaphoreTake(mutex_alteracoes, tempo_limite) == pdTRUE) {}

    ~BloqueioAlteracoes() {
        if (adquirido_) xSemaphoreGive(mutex_alteracoes);
    }

    bool adquirido() const { return adquirido_; }
    BloqueioAlteracoes(const BloqueioAlteracoes&) = delete;
    BloqueioAlteracoes& operator=(const BloqueioAlteracoes&) = delete;

private:
    bool adquirido_ = false;
};

PerfilWifi criar_perfil(const char* ssid, const char* senha) {
    PerfilWifi perfil{};
    std::strncpy(perfil.ssid, ssid, sizeof(perfil.ssid) - 1);
    std::strncpy(perfil.senha, senha, sizeof(perfil.senha) - 1);
    return perfil;
}

ConfiguracaoPerfis copiar_perfis() {
    portENTER_CRITICAL(&trava_perfis);
    const ConfiguracaoPerfis copia = perfis;
    portEXIT_CRITICAL(&trava_perfis);
    return copia;
}

void publicar_perfis(const ConfiguracaoPerfis& novos_perfis) {
    portENTER_CRITICAL(&trava_perfis);
    perfis = novos_perfis;
    portEXIT_CRITICAL(&trava_perfis);
}

EstadoServicoWifi copiar_estado_servico() {
    portENTER_CRITICAL(&trava_estado_servico);
    const EstadoServicoWifi copia = estado_servico;
    portEXIT_CRITICAL(&trava_estado_servico);
    return copia;
}

void definir_sessao(bool em_andamento, bool aguardando_autorizacao) {
    portENTER_CRITICAL(&trava_estado_servico);
    estado_servico.sessao_em_andamento = em_andamento;
    estado_servico.aguardando_autorizacao_secundaria = aguardando_autorizacao;
    portEXIT_CRITICAL(&trava_estado_servico);
}

void reiniciar_contador_tentativas() {
    portENTER_CRITICAL(&trava_estado_servico);
    estado_servico.tentativas_ultima_sessao = 0;
    portEXIT_CRITICAL(&trava_estado_servico);
}

void registrar_tentativa() {
    portENTER_CRITICAL(&trava_estado_servico);
    estado_servico.tentativas_ultima_sessao++;
    portEXIT_CRITICAL(&trava_estado_servico);
}

PerfilWifi obter_principal(const ConfiguracaoPerfis& configuracao_perfis) {
    if (configuracao_perfis.principal_alterada) return configuracao_perfis.principal;
    if (gerenciador_wifi_credenciais_validas(principal_firmware_cache.ssid,
                                              principal_firmware_cache.senha)) {
        return principal_firmware_cache;
    }
    return criar_perfil(configuracao::WIFI_PRINCIPAL_SSID,
                        configuracao::WIFI_PRINCIPAL_SENHA);
}

esp_err_t ler_texto_nvs(nvs_handle_t armazenamento, const char* chave,
                        char* destino, size_t capacidade) {
    size_t tamanho = capacidade;
    return nvs_get_str(armazenamento, chave, destino, &tamanho);
}

void apagar_chave_se_existir(nvs_handle_t armazenamento, const char* chave) {
    const esp_err_t erro = nvs_erase_key(armazenamento, chave);
    if (erro != ESP_OK && erro != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(ETIQUETA, "Não foi possível remover a chave NVS '%s': %s",
                 chave, esp_err_to_name(erro));
    }
}

esp_err_t carregar_cache_principal(PerfilWifi* destino) {
    if (destino == nullptr) return ESP_ERR_INVALID_ARG;
    nvs_handle_t armazenamento = 0;
    esp_err_t erro = nvs_open(ESPACO_NVS, NVS_READONLY, &armazenamento);
    if (erro != ESP_OK) return erro;
    PerfilWifi cache{};
    erro = ler_texto_nvs(armazenamento, "f_ssid", cache.ssid, sizeof(cache.ssid));
    if (erro == ESP_OK) {
        erro = ler_texto_nvs(armazenamento, "f_senha", cache.senha,
                             sizeof(cache.senha));
    }
    nvs_close(armazenamento);
    if (erro == ESP_OK &&
        gerenciador_wifi_credenciais_validas(cache.ssid, cache.senha)) {
        *destino = cache;
        return ESP_OK;
    }
    return erro == ESP_OK ? ESP_ERR_INVALID_RESPONSE : erro;
}

esp_err_t salvar_cache_principal(const PerfilWifi& origem) {
    nvs_handle_t armazenamento = 0;
    esp_err_t erro = nvs_open(ESPACO_NVS, NVS_READWRITE, &armazenamento);
    if (erro == ESP_OK) erro = nvs_set_str(armazenamento, "f_ssid", origem.ssid);
    if (erro == ESP_OK) erro = nvs_set_str(armazenamento, "f_senha", origem.senha);
    if (erro == ESP_OK) erro = nvs_commit(armazenamento);
    if (armazenamento != 0) nvs_close(armazenamento);
    return erro;
}

esp_err_t carregar_perfis(ConfiguracaoPerfis* destino) {
    if (destino == nullptr) return ESP_ERR_INVALID_ARG;
    *destino = {};

    nvs_handle_t armazenamento = 0;
    esp_err_t erro = nvs_open(ESPACO_NVS, NVS_READONLY, &armazenamento);
    if (erro == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (erro != ESP_OK) return erro;

    uint8_t versao = 0;
    if (nvs_get_u8(armazenamento, "versao", &versao) != ESP_OK ||
        versao != VERSAO_ARMAZENAMENTO) {
        nvs_close(armazenamento);
        ESP_LOGW(ETIQUETA,
                 "Configuração Wi-Fi salva ausente ou incompatível; usando padrões");
        return ESP_OK;
    }

    uint8_t valor = 0;
    if (nvs_get_u8(armazenamento, "principal", &valor) == ESP_OK && valor != 0) {
        PerfilWifi principal{};
        if (ler_texto_nvs(armazenamento, "p_ssid", principal.ssid,
                          sizeof(principal.ssid)) == ESP_OK &&
            ler_texto_nvs(armazenamento, "p_senha", principal.senha,
                          sizeof(principal.senha)) == ESP_OK &&
            gerenciador_wifi_credenciais_validas(principal.ssid, principal.senha)) {
            destino->principal_alterada = true;
            destino->principal = principal;
        }
    }

    valor = 0;
    if (nvs_get_u8(armazenamento, "automatico", &valor) == ESP_OK) {
        destino->automatico = valor != 0;
    }

    uint8_t quantidade = 0;
    if (nvs_get_u8(armazenamento, "quantidade", &quantidade) == ESP_OK) {
        quantidade = quantidade > destino->redes.size()
                         ? static_cast<uint8_t>(destino->redes.size())
                         : quantidade;
        for (uint8_t indice = 0; indice < quantidade; ++indice) {
            char chave_ssid[] = "u0_ssid";
            char chave_senha[] = "u0_pass";
            chave_ssid[1] = static_cast<char>('0' + indice);
            chave_senha[1] = static_cast<char>('0' + indice);
            PerfilWifi perfil{};
            if (ler_texto_nvs(armazenamento, chave_ssid, perfil.ssid,
                              sizeof(perfil.ssid)) == ESP_OK &&
                ler_texto_nvs(armazenamento, chave_senha, perfil.senha,
                              sizeof(perfil.senha)) == ESP_OK &&
                gerenciador_wifi_credenciais_validas(perfil.ssid, perfil.senha)) {
                destino->redes[destino->quantidade++] = perfil;
            }
        }
    }

    uint8_t tentativas = destino->politica.tentativas_por_rede;
    uint32_t tempo_tentativa = destino->politica.tempo_por_tentativa_ms;
    uint32_t intervalo = destino->politica.intervalo_entre_tentativas_ms;
    uint32_t tempo_sessao = destino->politica.tempo_limite_sessao_ms;
    if (nvs_get_u8(armazenamento, "tentativas", &tentativas) == ESP_OK) {
        destino->politica.tentativas_por_rede = tentativas;
    }
    if (nvs_get_u32(armazenamento, "tent_ms", &tempo_tentativa) == ESP_OK) {
        destino->politica.tempo_por_tentativa_ms = tempo_tentativa;
    }
    if (nvs_get_u32(armazenamento, "intervalo_ms", &intervalo) == ESP_OK) {
        destino->politica.intervalo_entre_tentativas_ms = intervalo;
    }
    if (nvs_get_u32(armazenamento, "sessao_ms", &tempo_sessao) == ESP_OK) {
        destino->politica.tempo_limite_sessao_ms = tempo_sessao;
    }
    if (!servico_wifi_politica_valida(destino->politica)) {
        ESP_LOGW(ETIQUETA, "Política Wi-Fi salva inválida; usando valores padrão");
        destino->politica = {};
    }

    nvs_close(armazenamento);
    return ESP_OK;
}

esp_err_t salvar_perfis(const ConfiguracaoPerfis& origem) {
    nvs_handle_t armazenamento = 0;
    esp_err_t erro = nvs_open(ESPACO_NVS, NVS_READWRITE, &armazenamento);
    if (erro == ESP_OK) erro = nvs_set_u8(armazenamento, "versao", VERSAO_ARMAZENAMENTO);
    if (erro == ESP_OK) {
        erro = nvs_set_u8(armazenamento, "principal",
                          origem.principal_alterada ? 1 : 0);
    }
    if (erro == ESP_OK) {
        erro = nvs_set_u8(armazenamento, "automatico", origem.automatico ? 1 : 0);
    }
    if (erro == ESP_OK) erro = nvs_set_u8(armazenamento, "quantidade", origem.quantidade);
    if (erro == ESP_OK) {
        erro = nvs_set_u8(armazenamento, "tentativas",
                          origem.politica.tentativas_por_rede);
    }
    if (erro == ESP_OK) {
        erro = nvs_set_u32(armazenamento, "tent_ms",
                           origem.politica.tempo_por_tentativa_ms);
    }
    if (erro == ESP_OK) {
        erro = nvs_set_u32(armazenamento, "intervalo_ms",
                           origem.politica.intervalo_entre_tentativas_ms);
    }
    if (erro == ESP_OK) {
        erro = nvs_set_u32(armazenamento, "sessao_ms",
                           origem.politica.tempo_limite_sessao_ms);
    }

    if (erro == ESP_OK && origem.principal_alterada) {
        erro = nvs_set_str(armazenamento, "p_ssid", origem.principal.ssid);
        if (erro == ESP_OK) {
            erro = nvs_set_str(armazenamento, "p_senha", origem.principal.senha);
        }
    } else if (erro == ESP_OK) {
        apagar_chave_se_existir(armazenamento, "p_ssid");
        apagar_chave_se_existir(armazenamento, "p_senha");
    }

    for (size_t indice = 0; erro == ESP_OK && indice < origem.redes.size(); ++indice) {
        char chave_ssid[] = "u0_ssid";
        char chave_senha[] = "u0_pass";
        chave_ssid[1] = static_cast<char>('0' + indice);
        chave_senha[1] = static_cast<char>('0' + indice);
        if (indice < origem.quantidade) {
            erro = nvs_set_str(armazenamento, chave_ssid, origem.redes[indice].ssid);
            if (erro == ESP_OK) {
                erro = nvs_set_str(armazenamento, chave_senha,
                                   origem.redes[indice].senha);
            }
        } else {
            apagar_chave_se_existir(armazenamento, chave_ssid);
            apagar_chave_se_existir(armazenamento, chave_senha);
        }
    }

    if (erro == ESP_OK) erro = nvs_commit(armazenamento);
    if (armazenamento != 0) nvs_close(armazenamento);
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Falha ao persistir os perfis Wi-Fi: %s",
                 esp_err_to_name(erro));
    }
    return erro;
}

int64_t agora_ms() {
    return esp_timer_get_time() / 1000;
}

esp_err_t aplicar_perfil(const PerfilWifi& perfil,
                         const PoliticaConexaoWifi& politica,
                         int64_t instante_limite_ms) {
    esp_err_t ultimo_erro = ESP_ERR_TIMEOUT;
    for (uint8_t tentativa = 1; tentativa <= politica.tentativas_por_rede;
         ++tentativa) {
        const int64_t restante_ms = instante_limite_ms - agora_ms();
        if (restante_ms <= 0) return ESP_ERR_TIMEOUT;

        const uint32_t tempo_tentativa_ms = std::min(
            politica.tempo_por_tentativa_ms,
            static_cast<uint32_t>(restante_ms));
        ESP_LOGI(ETIQUETA,
                 "Rede '%s': tentativa %u de %u (limite desta tentativa: %lu ms)",
                 perfil.ssid, static_cast<unsigned>(tentativa),
                 static_cast<unsigned>(politica.tentativas_por_rede),
                 static_cast<unsigned long>(tempo_tentativa_ms));
        registrar_tentativa();
        ultimo_erro = gerenciador_wifi_conectar(
            perfil.ssid, perfil.senha,
            std::max<TickType_t>(1, pdMS_TO_TICKS(tempo_tentativa_ms)));
        if (ultimo_erro == ESP_OK) return ESP_OK;

        ESP_LOGW(ETIQUETA, "Rede '%s': tentativa %u falhou: %s", perfil.ssid,
                 static_cast<unsigned>(tentativa), esp_err_to_name(ultimo_erro));
        if (tentativa == politica.tentativas_por_rede) break;

        const int64_t restante_apos_tentativa = instante_limite_ms - agora_ms();
        if (restante_apos_tentativa <= 0) return ESP_ERR_TIMEOUT;
        const uint32_t pausa_ms = std::min(
            politica.intervalo_entre_tentativas_ms,
            static_cast<uint32_t>(restante_apos_tentativa));
        vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(pausa_ms)));
    }
    return ultimo_erro;
}

esp_err_t testar_perfil_e_desligar(const PerfilWifi& perfil) {
    const ConfiguracaoPerfis copia = copiar_perfis();
    reiniciar_contador_tentativas();
    const esp_err_t resultado = aplicar_perfil(
        perfil, copia.politica,
        agora_ms() + copia.politica.tempo_limite_sessao_ms);
    if (resultado == ESP_OK) {
        const EstadoWifi estado = gerenciador_wifi_obter_estado();
        ESP_LOGI(ETIQUETA, "Rede '%s' validada; RSSI=%d dBm, canal=%u",
                 perfil.ssid, estado.rssi_dbm, static_cast<unsigned>(estado.canal));
    }
    const esp_err_t erro_desligar = gerenciador_wifi_desativar(TEMPO_DESLIGAMENTO);
    return resultado != ESP_OK ? resultado : erro_desligar;
}

bool servico_pronto_sem_sessao() {
    const EstadoServicoWifi estado = copiar_estado_servico();
    return estado.iniciado && !estado.sessao_em_andamento;
}
}  // namespace

bool servico_wifi_politica_valida(const PoliticaConexaoWifi& politica) {
    return politica.tentativas_por_rede >= configuracao::TENTATIVAS_WIFI_MINIMAS &&
           politica.tentativas_por_rede <= configuracao::TENTATIVAS_WIFI_MAXIMAS &&
           politica.tempo_por_tentativa_ms >=
               configuracao::TEMPO_TENTATIVA_WIFI_MINIMO_MS &&
           politica.tempo_por_tentativa_ms <=
               configuracao::TEMPO_TENTATIVA_WIFI_MAXIMO_MS &&
           politica.intervalo_entre_tentativas_ms >=
               configuracao::INTERVALO_TENTATIVAS_WIFI_MINIMO_MS &&
           politica.intervalo_entre_tentativas_ms <=
               configuracao::INTERVALO_TENTATIVAS_WIFI_MAXIMO_MS &&
           politica.tempo_limite_sessao_ms >=
               configuracao::TEMPO_LIMITE_SESSAO_WIFI_MINIMO_MS &&
           politica.tempo_limite_sessao_ms <=
               configuracao::TEMPO_LIMITE_SESSAO_WIFI_MAXIMO_MS &&
           politica.tempo_limite_sessao_ms >= politica.tempo_por_tentativa_ms;
}

esp_err_t servico_wifi_iniciar() {
    if (copiar_estado_servico().iniciado) return ESP_OK;
    if (mutex_alteracoes == nullptr) mutex_alteracoes = xSemaphoreCreateMutex();
    if (mutex_alteracoes == nullptr) return ESP_ERR_NO_MEM;

    BloqueioAlteracoes bloqueio;
    if (!bloqueio.adquirido()) return ESP_ERR_TIMEOUT;
    if (copiar_estado_servico().iniciado) return ESP_OK;

    ConfiguracaoPerfis carregados{};
    esp_err_t erro = carregar_perfis(&carregados);
    if (erro != ESP_OK) return erro;

    const PerfilWifi principal_compilada = criar_perfil(
        configuracao::WIFI_PRINCIPAL_SSID, configuracao::WIFI_PRINCIPAL_SENHA);
    if (gerenciador_wifi_credenciais_validas(principal_compilada.ssid,
                                              principal_compilada.senha)) {
        principal_firmware_cache = principal_compilada;
        PerfilWifi cache_existente{};
        const esp_err_t erro_cache = carregar_cache_principal(&cache_existente);
        if (erro_cache != ESP_OK ||
            std::strcmp(cache_existente.ssid, principal_firmware_cache.ssid) != 0 ||
            std::strcmp(cache_existente.senha, principal_firmware_cache.senha) != 0) {
            const esp_err_t erro_salvar = salvar_cache_principal(principal_firmware_cache);
            if (erro_salvar != ESP_OK) {
                ESP_LOGW(ETIQUETA,
                         "Não foi possível preservar a rede principal na NVS: %s",
                         esp_err_to_name(erro_salvar));
            }
        }
    } else {
        erro = carregar_cache_principal(&principal_firmware_cache);
        if (erro == ESP_OK) {
            ESP_LOGI(ETIQUETA,
                     "Rede principal recuperada da NVS para este firmware OTA");
        }
    }

    const PerfilWifi principal = obter_principal(carregados);
    if (!gerenciador_wifi_credenciais_validas(principal.ssid, principal.senha)) {
        ESP_LOGE(ETIQUETA, "A rede Wi-Fi principal não está configurada");
        return ESP_ERR_INVALID_STATE;
    }

    erro = gerenciador_wifi_iniciar(principal.ssid, principal.senha);
    if (erro != ESP_OK) return erro;
    publicar_perfis(carregados);
    portENTER_CRITICAL(&trava_estado_servico);
    estado_servico.iniciado = true;
    portEXIT_CRITICAL(&trava_estado_servico);
    ESP_LOGI(ETIQUETA,
             "Perfis Wi-Fi prontos; o rádio será usado somente durante operações de rede");
    return ESP_OK;
}

esp_err_t servico_wifi_abrir_sessao() {
    if (mutex_alteracoes == nullptr) return ESP_ERR_INVALID_STATE;
    BloqueioAlteracoes bloqueio;
    if (!bloqueio.adquirido()) return ESP_ERR_TIMEOUT;
    if (!servico_pronto_sem_sessao()) return ESP_ERR_INVALID_STATE;

    definir_sessao(true, false);
    reiniciar_contador_tentativas();
    if (gerenciador_wifi_esta_conectado()) return ESP_OK;

    const ConfiguracaoPerfis copia = copiar_perfis();
    const int64_t instante_limite =
        agora_ms() + copia.politica.tempo_limite_sessao_ms;
    const PerfilWifi principal = obter_principal(copia);
    const bool usar_autorizacao = existe_perfil_autorizado_uma_vez;
    const PerfilWifi perfil_autorizado = perfil_autorizado_uma_vez;
    existe_perfil_autorizado_uma_vez = false;
    perfil_autorizado_uma_vez = {};

    esp_err_t ultimo_erro =
        aplicar_perfil(principal, copia.politica, instante_limite);
    if (ultimo_erro == ESP_OK) {
        ESP_LOGI(ETIQUETA, "Sessão de rede aberta pela rede principal '%s'",
                 principal.ssid);
        return ESP_OK;
    }

    if (usar_autorizacao &&
        std::strcmp(principal.ssid, perfil_autorizado.ssid) != 0) {
        ESP_LOGI(ETIQUETA, "Usando autorização única para a rede '%s'",
                 perfil_autorizado.ssid);
        ultimo_erro =
            aplicar_perfil(perfil_autorizado, copia.politica, instante_limite);
        if (ultimo_erro == ESP_OK) return ESP_OK;
    }

    if (!copia.automatico || copia.quantidade == 0) {
        const bool aguardar = copia.quantidade > 0;
        definir_sessao(false, aguardar);
        const esp_err_t erro_desligar =
            gerenciador_wifi_desativar(TEMPO_DESLIGAMENTO);
        if (erro_desligar != ESP_OK) {
            ESP_LOGW(ETIQUETA, "Falha ao desligar rádio após conexão recusada: %s",
                     esp_err_to_name(erro_desligar));
        }
        if (aguardar) {
            ESP_LOGW(ETIQUETA,
                     "Rede principal indisponível; redes secundárias aguardam autorização");
        }
        return ultimo_erro;
    }

    ESP_LOGW(ETIQUETA,
             "Rede principal indisponível; tentando %u rede(s) secundária(s)",
             static_cast<unsigned>(copia.quantidade));
    for (uint8_t indice = 0; indice < copia.quantidade; ++indice) {
        if ((usar_autorizacao &&
             std::strcmp(copia.redes[indice].ssid, perfil_autorizado.ssid) == 0) ||
            std::strcmp(copia.redes[indice].ssid, principal.ssid) == 0) {
            continue;
        }
        ultimo_erro =
            aplicar_perfil(copia.redes[indice], copia.politica, instante_limite);
        if (ultimo_erro == ESP_OK) {
            ESP_LOGI(ETIQUETA, "Sessão de rede aberta por '%s'",
                     copia.redes[indice].ssid);
            return ESP_OK;
        }
        if (agora_ms() >= instante_limite) break;
    }

    definir_sessao(false, false);
    const esp_err_t erro_desligar = gerenciador_wifi_desativar(TEMPO_DESLIGAMENTO);
    if (erro_desligar != ESP_OK) {
        ESP_LOGW(ETIQUETA, "Falha ao desligar rádio após tentativas: %s",
                 esp_err_to_name(erro_desligar));
    }
    ESP_LOGE(ETIQUETA, "Nenhuma rede Wi-Fi autorizada está disponível");
    return ultimo_erro;
}

void servico_wifi_encerrar_sessao() {
    if (mutex_alteracoes == nullptr) return;
    BloqueioAlteracoes bloqueio;
    if (!bloqueio.adquirido()) return;
    if (!copiar_estado_servico().iniciado) return;
    const esp_err_t erro = gerenciador_wifi_desativar(TEMPO_DESLIGAMENTO);
    definir_sessao(false, false);
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Falha ao encerrar sessão Wi-Fi: %s",
                 esp_err_to_name(erro));
    }
}

esp_err_t servico_wifi_conectar_rede_informada(const char* ssid,
                                                const char* senha) {
    if (!gerenciador_wifi_credenciais_validas(ssid, senha)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (mutex_alteracoes == nullptr) return ESP_ERR_INVALID_STATE;
    BloqueioAlteracoes bloqueio;
    if (!bloqueio.adquirido()) return ESP_ERR_TIMEOUT;
    if (!servico_pronto_sem_sessao()) return ESP_ERR_INVALID_STATE;

    const PerfilWifi perfil = criar_perfil(ssid, senha);
    const esp_err_t erro = testar_perfil_e_desligar(perfil);
    if (erro == ESP_OK) {
        perfil_autorizado_uma_vez = perfil;
        existe_perfil_autorizado_uma_vez = true;
        definir_sessao(false, false);
    }
    return erro;
}

esp_err_t servico_wifi_conectar_rede_salva(std::size_t indice) {
    if (mutex_alteracoes == nullptr) return ESP_ERR_INVALID_STATE;
    BloqueioAlteracoes bloqueio;
    if (!bloqueio.adquirido()) return ESP_ERR_TIMEOUT;
    if (!servico_pronto_sem_sessao()) return ESP_ERR_INVALID_STATE;
    const ConfiguracaoPerfis copia = copiar_perfis();
    if (indice >= copia.quantidade) return ESP_ERR_INVALID_ARG;

    const esp_err_t erro = testar_perfil_e_desligar(copia.redes[indice]);
    if (erro == ESP_OK) {
        perfil_autorizado_uma_vez = copia.redes[indice];
        existe_perfil_autorizado_uma_vez = true;
        definir_sessao(false, false);
    }
    return erro;
}

esp_err_t servico_wifi_salvar_rede_usuario(const char* ssid, const char* senha) {
    if (!gerenciador_wifi_credenciais_validas(ssid, senha)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (mutex_alteracoes == nullptr) return ESP_ERR_INVALID_STATE;
    BloqueioAlteracoes bloqueio;
    if (!bloqueio.adquirido()) return ESP_ERR_TIMEOUT;
    if (!servico_pronto_sem_sessao()) return ESP_ERR_INVALID_STATE;

    ConfiguracaoPerfis nova = copiar_perfis();
    const PerfilWifi principal = obter_principal(nova);
    if (std::strcmp(principal.ssid, ssid) == 0) return ESP_ERR_INVALID_STATE;

    for (uint8_t indice = 0; indice < nova.quantidade; ++indice) {
        if (std::strcmp(nova.redes[indice].ssid, ssid) == 0) {
            nova.redes[indice] = criar_perfil(ssid, senha);
            const esp_err_t erro = salvar_perfis(nova);
            if (erro == ESP_OK) {
                publicar_perfis(nova);
                ESP_LOGI(ETIQUETA,
                         "Credenciais da rede secundária '%s' atualizadas", ssid);
            }
            return erro;
        }
    }

    if (nova.quantidade >= nova.redes.size()) return ESP_ERR_NO_MEM;
    nova.redes[nova.quantidade++] = criar_perfil(ssid, senha);
    const esp_err_t erro = salvar_perfis(nova);
    if (erro == ESP_OK) {
        publicar_perfis(nova);
        ESP_LOGI(ETIQUETA, "Rede secundária '%s' salva (%u de %u)", ssid,
                 static_cast<unsigned>(nova.quantidade),
                 static_cast<unsigned>(nova.redes.size()));
    }
    return erro;
}

esp_err_t servico_wifi_excluir_rede_usuario(std::size_t indice) {
    if (mutex_alteracoes == nullptr) return ESP_ERR_INVALID_STATE;
    BloqueioAlteracoes bloqueio;
    if (!bloqueio.adquirido()) return ESP_ERR_TIMEOUT;
    if (!servico_pronto_sem_sessao()) return ESP_ERR_INVALID_STATE;

    ConfiguracaoPerfis nova = copiar_perfis();
    if (indice >= nova.quantidade) return ESP_ERR_INVALID_ARG;
    const PerfilWifi removida = nova.redes[indice];
    for (size_t atual = indice; atual + 1 < nova.quantidade; ++atual) {
        nova.redes[atual] = nova.redes[atual + 1];
    }
    nova.redes[--nova.quantidade] = {};
    const esp_err_t erro = salvar_perfis(nova);
    if (erro == ESP_OK) {
        publicar_perfis(nova);
        if (existe_perfil_autorizado_uma_vez &&
            std::strcmp(perfil_autorizado_uma_vez.ssid, removida.ssid) == 0) {
            existe_perfil_autorizado_uma_vez = false;
            perfil_autorizado_uma_vez = {};
        }
        ESP_LOGI(ETIQUETA, "Rede secundária '%s' excluída", removida.ssid);
    }
    return erro;
}

bool servico_wifi_validar_permissao(const char* senha_permissao) {
    if (senha_permissao == nullptr ||
        configuracao::SENHA_PERMISSAO_WIFI[0] == '\0') {
        return false;
    }
    const size_t recebido = std::strlen(senha_permissao);
    const size_t esperado = std::strlen(configuracao::SENHA_PERMISSAO_WIFI);
    size_t diferenca = recebido ^ esperado;
    const size_t maximo = recebido > esperado ? recebido : esperado;
    for (size_t indice = 0; indice < maximo; ++indice) {
        const unsigned char a = indice < recebido ? senha_permissao[indice] : 0;
        const unsigned char b =
            indice < esperado ? configuracao::SENHA_PERMISSAO_WIFI[indice] : 0;
        diferenca |= a ^ b;
    }
    return diferenca == 0;
}

esp_err_t servico_wifi_alterar_rede_principal(const char* ssid,
                                              const char* senha,
                                              const char* senha_permissao) {
    if (!servico_wifi_validar_permissao(senha_permissao)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!gerenciador_wifi_credenciais_validas(ssid, senha)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (mutex_alteracoes == nullptr) return ESP_ERR_INVALID_STATE;
    BloqueioAlteracoes bloqueio;
    if (!bloqueio.adquirido()) return ESP_ERR_TIMEOUT;
    if (!servico_pronto_sem_sessao()) return ESP_ERR_INVALID_STATE;

    const PerfilWifi nova_principal = criar_perfil(ssid, senha);
    const esp_err_t erro_conexao = testar_perfil_e_desligar(nova_principal);
    if (erro_conexao != ESP_OK) return erro_conexao;

    ConfiguracaoPerfis nova = copiar_perfis();
    nova.principal_alterada = true;
    nova.principal = nova_principal;
    const esp_err_t erro = salvar_perfis(nova);
    if (erro == ESP_OK) {
        publicar_perfis(nova);
        ESP_LOGI(ETIQUETA, "Rede principal alterada para '%s'", ssid);
    }
    return erro;
}

esp_err_t servico_wifi_restaurar_rede_principal(
    const char* senha_permissao) {
    if (!servico_wifi_validar_permissao(senha_permissao)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (mutex_alteracoes == nullptr) return ESP_ERR_INVALID_STATE;
    BloqueioAlteracoes bloqueio;
    if (!bloqueio.adquirido()) return ESP_ERR_TIMEOUT;
    if (!servico_pronto_sem_sessao()) return ESP_ERR_INVALID_STATE;

    const PerfilWifi original = criar_perfil(configuracao::WIFI_PRINCIPAL_SSID,
                                              configuracao::WIFI_PRINCIPAL_SENHA);
    if (!gerenciador_wifi_credenciais_validas(original.ssid, original.senha)) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t erro_conexao = testar_perfil_e_desligar(original);
    if (erro_conexao != ESP_OK) return erro_conexao;

    ConfiguracaoPerfis nova = copiar_perfis();
    nova.principal_alterada = false;
    nova.principal = {};
    const esp_err_t erro = salvar_perfis(nova);
    if (erro == ESP_OK) {
        publicar_perfis(nova);
        ESP_LOGI(ETIQUETA,
                 "Rede principal restaurada para a definição do firmware");
    }
    return erro;
}

esp_err_t servico_wifi_definir_conexao_automatica(
    bool habilitar, const char* senha_permissao) {
    if (!servico_wifi_validar_permissao(senha_permissao)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (mutex_alteracoes == nullptr) return ESP_ERR_INVALID_STATE;
    BloqueioAlteracoes bloqueio;
    if (!bloqueio.adquirido()) return ESP_ERR_TIMEOUT;
    if (!servico_pronto_sem_sessao()) return ESP_ERR_INVALID_STATE;

    ConfiguracaoPerfis nova = copiar_perfis();
    nova.automatico = habilitar;
    const esp_err_t erro = salvar_perfis(nova);
    if (erro == ESP_OK) {
        publicar_perfis(nova);
        ESP_LOGI(ETIQUETA, "Conexão automática às redes secundárias %s",
                 habilitar ? "habilitada" : "desabilitada");
    }
    return erro;
}

esp_err_t servico_wifi_definir_politica(
    const PoliticaConexaoWifi& politica, const char* senha_permissao) {
    if (!servico_wifi_validar_permissao(senha_permissao)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!servico_wifi_politica_valida(politica)) return ESP_ERR_INVALID_ARG;
    if (mutex_alteracoes == nullptr) return ESP_ERR_INVALID_STATE;
    BloqueioAlteracoes bloqueio;
    if (!bloqueio.adquirido()) return ESP_ERR_TIMEOUT;
    if (!servico_pronto_sem_sessao()) return ESP_ERR_INVALID_STATE;

    ConfiguracaoPerfis nova = copiar_perfis();
    nova.politica = politica;
    const esp_err_t erro = salvar_perfis(nova);
    if (erro == ESP_OK) {
        publicar_perfis(nova);
        ESP_LOGI(ETIQUETA,
                 "Política Wi-Fi atualizada: %u tentativa(s), %lu ms por tentativa, "
                 "%lu ms entre tentativas, sessão de %lu ms",
                 static_cast<unsigned>(politica.tentativas_por_rede),
                 static_cast<unsigned long>(politica.tempo_por_tentativa_ms),
                 static_cast<unsigned long>(
                     politica.intervalo_entre_tentativas_ms),
                 static_cast<unsigned long>(politica.tempo_limite_sessao_ms));
    }
    return erro;
}

ResumoWifi servico_wifi_obter_resumo() {
    ResumoWifi resumo{};
    const EstadoServicoWifi estado_local = copiar_estado_servico();
    if (!estado_local.iniciado) return resumo;

    const ConfiguracaoPerfis copia = copiar_perfis();
    const EstadoWifi estado = gerenciador_wifi_obter_estado();
    const PerfilWifi principal = obter_principal(copia);
    resumo.inicializado = true;
    resumo.radio_ativo = estado.ativo;
    resumo.conectado = estado.conectado;
    resumo.aguardando_autorizacao_secundaria =
        estado_local.aguardando_autorizacao_secundaria;
    resumo.principal_alterada = copia.principal_alterada;
    resumo.conectar_secundarias_automaticamente = copia.automatico;
    resumo.ultimo_erro = estado.ultimo_erro;
    resumo.rssi_dbm = estado.rssi_dbm;
    resumo.canal = estado.canal;
    resumo.ultimo_motivo_desconexao = estado.ultimo_motivo_desconexao;
    resumo.tentativas_conexao = estado.tentativas_conexao;
    resumo.conexoes_bem_sucedidas = estado.conexoes_bem_sucedidas;
    resumo.desconexoes_inesperadas = estado.desconexoes;
    resumo.tentativas_ultima_sessao =
        estado_local.tentativas_ultima_sessao;
    resumo.sessao_em_andamento = estado_local.sessao_em_andamento;
    resumo.politica = copia.politica;
    std::strncpy(resumo.rede_ativa, estado.ssid_atual,
                 sizeof(resumo.rede_ativa) - 1);
    std::strncpy(resumo.rede_principal, principal.ssid,
                 sizeof(resumo.rede_principal) - 1);
    resumo.quantidade_redes_usuario = copia.quantidade;
    for (uint8_t indice = 0; indice < copia.quantidade; ++indice) {
        std::strncpy(resumo.redes_usuario[indice], copia.redes[indice].ssid,
                     sizeof(resumo.redes_usuario[indice]) - 1);
    }
    return resumo;
}
