#include "gerenciadores/gerenciador_wifi.h"

#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "nucleo/configuracao_ota.h"

namespace {
constexpr char ETIQUETA[] = "gerenciador_wifi";
constexpr EventBits_t BIT_CONECTADO = BIT0;
constexpr EventBits_t BIT_FALHA = BIT1;

EventGroupHandle_t eventos_wifi = nullptr;
esp_event_handler_instance_t tratador_wifi = nullptr;
esp_event_handler_instance_t tratador_ip = nullptr;
EstadoWifi estado{};
portMUX_TYPE trava_estado = portMUX_INITIALIZER_UNLOCKED;

void definir_erro(esp_err_t erro) {
    portENTER_CRITICAL(&trava_estado);
    estado.ultimo_erro = erro;
    portEXIT_CRITICAL(&trava_estado);
}

void tratar_evento(void*, esp_event_base_t base, int32_t identificador, void* dados) {
    if (base == WIFI_EVENT && identificador == WIFI_EVENT_STA_START) {
        esp_err_t erro = esp_wifi_connect();
        if (erro != ESP_OK) {
            definir_erro(erro);
            xEventGroupSetBits(eventos_wifi, BIT_FALHA);
        }
        return;
    }

    if (base == WIFI_EVENT && identificador == WIFI_EVENT_STA_DISCONNECTED) {
        portENTER_CRITICAL(&trava_estado);
        estado.conectado = false;
        estado.desconexoes++;
        estado.tentativas_conexao++;
        const uint32_t tentativa = estado.tentativas_conexao;
        portEXIT_CRITICAL(&trava_estado);
        xEventGroupClearBits(eventos_wifi, BIT_CONECTADO);

        if (tentativa <= static_cast<uint32_t>(configuracao::MAXIMO_TENTATIVAS_WIFI)) {
            ESP_LOGW(ETIQUETA, "Wi-Fi desconectado; tentativa %lu de %d",
                     static_cast<unsigned long>(tentativa),
                     configuracao::MAXIMO_TENTATIVAS_WIFI);
            esp_err_t erro = esp_wifi_connect();
            if (erro != ESP_OK) definir_erro(erro);
        } else {
            ESP_LOGE(ETIQUETA, "Limite de tentativas de conexão atingido");
            definir_erro(ESP_ERR_TIMEOUT);
            xEventGroupSetBits(eventos_wifi, BIT_FALHA);
        }
        return;
    }

    if (base == IP_EVENT && identificador == IP_EVENT_STA_GOT_IP) {
        const auto* evento_ip = static_cast<const ip_event_got_ip_t*>(dados);
        portENTER_CRITICAL(&trava_estado);
        estado.conectado = true;
        estado.tentativas_conexao = 0;
        estado.ultimo_erro = ESP_OK;
        portEXIT_CRITICAL(&trava_estado);
        xEventGroupClearBits(eventos_wifi, BIT_FALHA);
        xEventGroupSetBits(eventos_wifi, BIT_CONECTADO);
        ESP_LOGI(ETIQUETA, "Conectado; IP=" IPSTR, IP2STR(&evento_ip->ip_info.ip));
    }
}

esp_err_t iniciar_nvs() {
    esp_err_t erro = nvs_flash_init();
    if (erro == ESP_ERR_NVS_NO_FREE_PAGES || erro == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(ETIQUETA, "NVS incompatível; apagando e reinicializando");
        erro = nvs_flash_erase();
        if (erro == ESP_OK) erro = nvs_flash_init();
    }
    return erro;
}

esp_err_t salvar_credenciais(const char* ssid, const char* senha) {
    nvs_handle_t armazenamento = 0;
    esp_err_t erro = nvs_open("wifi_ota", NVS_READWRITE, &armazenamento);
    if (erro == ESP_OK) erro = nvs_set_str(armazenamento, "ssid", ssid);
    if (erro == ESP_OK) erro = nvs_set_str(armazenamento, "senha", senha);
    if (erro == ESP_OK) erro = nvs_commit(armazenamento);
    if (erro == ESP_OK) ESP_LOGI(ETIQUETA, "Credenciais Wi-Fi salvas na NVS");
    if (armazenamento != 0) nvs_close(armazenamento);
    return erro;
}

esp_err_t carregar_credenciais(char* ssid, size_t capacidade_ssid,
                               char* senha, size_t capacidade_senha) {
    nvs_handle_t armazenamento = 0;
    esp_err_t erro = nvs_open("wifi_ota", NVS_READONLY, &armazenamento);
    size_t tamanho_ssid = capacidade_ssid;
    size_t tamanho_senha = capacidade_senha;
    if (erro == ESP_OK) erro = nvs_get_str(armazenamento, "ssid", ssid, &tamanho_ssid);
    if (erro == ESP_OK) erro = nvs_get_str(armazenamento, "senha", senha, &tamanho_senha);
    if (armazenamento != 0) nvs_close(armazenamento);
    return erro;
}
}  // namespace

esp_err_t gerenciador_wifi_iniciar(const char* ssid, const char* senha) {
    if (ssid == nullptr || senha == nullptr) return ESP_ERR_INVALID_ARG;
    if (estado.inicializado) return ESP_OK;

    esp_err_t erro = iniciar_nvs();
    if (erro != ESP_OK) return erro;

    char ssid_efetivo[33]{};
    char senha_efetiva[64]{};
    if (ssid[0] != '\0') {
        if (std::strlen(ssid) > 32 || std::strlen(senha) < 8 || std::strlen(senha) > 63) {
            ESP_LOGE(ETIQUETA, "Credenciais Wi-Fi fora dos limites");
            return ESP_ERR_INVALID_ARG;
        }
        std::strcpy(ssid_efetivo, ssid);
        std::strcpy(senha_efetiva, senha);
        erro = salvar_credenciais(ssid_efetivo, senha_efetiva);
    } else {
        erro = carregar_credenciais(ssid_efetivo, sizeof(ssid_efetivo),
                                    senha_efetiva, sizeof(senha_efetiva));
        if (erro == ESP_OK) ESP_LOGI(ETIQUETA, "Credenciais Wi-Fi carregadas da NVS");
    }
    if (erro != ESP_OK || ssid_efetivo[0] == '\0') {
        ESP_LOGE(ETIQUETA, "Credenciais Wi-Fi não configuradas na NVS");
        return erro == ESP_OK ? ESP_ERR_NOT_FOUND : erro;
    }

    erro = esp_netif_init();
    if (erro != ESP_OK && erro != ESP_ERR_INVALID_STATE) return erro;
    erro = esp_event_loop_create_default();
    if (erro != ESP_OK && erro != ESP_ERR_INVALID_STATE) return erro;
    if (esp_netif_create_default_wifi_sta() == nullptr) return ESP_ERR_NO_MEM;

    eventos_wifi = xEventGroupCreate();
    if (eventos_wifi == nullptr) return ESP_ERR_NO_MEM;

    wifi_init_config_t configuracao_wifi = WIFI_INIT_CONFIG_DEFAULT();
    erro = esp_wifi_init(&configuracao_wifi);
    if (erro != ESP_OK) return erro;

    erro = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                &tratar_evento, nullptr, &tratador_wifi);
    if (erro == ESP_OK) {
        erro = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                    &tratar_evento, nullptr, &tratador_ip);
    }
    if (erro != ESP_OK) return erro;

    wifi_config_t parametros{};
    std::strncpy(reinterpret_cast<char*>(parametros.sta.ssid), ssid_efetivo,
                 sizeof(parametros.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(parametros.sta.password), senha_efetiva,
                 sizeof(parametros.sta.password) - 1);
    parametros.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    parametros.sta.pmf_cfg.capable = true;
    parametros.sta.pmf_cfg.required = false;

    erro = esp_wifi_set_mode(WIFI_MODE_STA);
    if (erro == ESP_OK) erro = esp_wifi_set_config(WIFI_IF_STA, &parametros);
    if (erro == ESP_OK) erro = esp_wifi_start();
    if (erro != ESP_OK) {
        definir_erro(erro);
        ESP_LOGE(ETIQUETA, "Falha ao iniciar Wi-Fi: %s", esp_err_to_name(erro));
        return erro;
    }

    portENTER_CRITICAL(&trava_estado);
    estado.inicializado = true;
    estado.ultimo_erro = ESP_OK;
    portEXIT_CRITICAL(&trava_estado);
    ESP_LOGI(ETIQUETA, "Wi-Fi iniciado para a rede '%s'", ssid_efetivo);
    return ESP_OK;
}

esp_err_t gerenciador_wifi_aguardar_conexao(TickType_t tempo_limite) {
    if (!estado.inicializado || eventos_wifi == nullptr) return ESP_ERR_INVALID_STATE;
    EventBits_t bits = xEventGroupWaitBits(eventos_wifi, BIT_CONECTADO | BIT_FALHA,
                                           pdFALSE, pdFALSE, tempo_limite);
    if ((bits & BIT_CONECTADO) != 0) return ESP_OK;
    return ESP_ERR_TIMEOUT;
}

esp_err_t gerenciador_wifi_reconectar() {
    if (!estado.inicializado) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&trava_estado);
    estado.tentativas_conexao = 0;
    portEXIT_CRITICAL(&trava_estado);
    xEventGroupClearBits(eventos_wifi, BIT_FALHA);
    esp_err_t erro = esp_wifi_connect();
    if (erro != ESP_OK) definir_erro(erro);
    return erro;
}

bool gerenciador_wifi_esta_conectado() {
    portENTER_CRITICAL(&trava_estado);
    const bool conectado = estado.conectado;
    portEXIT_CRITICAL(&trava_estado);
    return conectado;
}

EstadoWifi gerenciador_wifi_obter_estado() {
    portENTER_CRITICAL(&trava_estado);
    const EstadoWifi copia = estado;
    portEXIT_CRITICAL(&trava_estado);
    return copia;
}
