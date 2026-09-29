#include "gerenciadores/gerenciador_wifi.h"

#include <algorithm>
#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nucleo/configuracao_wifi.h"

namespace {
constexpr char ETIQUETA[] = "gerenciador_wifi";
constexpr EventBits_t BIT_CONECTADO = BIT0;
constexpr EventBits_t BIT_FALHA = BIT1;
constexpr EventBits_t BIT_DESCONECTADO = BIT2;
constexpr EventBits_t BIT_ATIVO = BIT3;
constexpr TickType_t TEMPO_ATIVACAO = pdMS_TO_TICKS(2000);

EventGroupHandle_t eventos_wifi = nullptr;
esp_event_handler_instance_t tratador_wifi = nullptr;
esp_event_handler_instance_t tratador_ip = nullptr;
esp_netif_t* interface_estacao = nullptr;
EstadoWifi estado{};
portMUX_TYPE trava_estado = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t mutex_operacao = nullptr;
bool desconexao_planejada = false;
bool driver_wifi_inicializado = false;

void liberar_inicializacao_parcial() {
    if (tratador_ip != nullptr) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              tratador_ip);
        tratador_ip = nullptr;
    }
    if (tratador_wifi != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              tratador_wifi);
        tratador_wifi = nullptr;
    }
    if (driver_wifi_inicializado) {
        esp_wifi_deinit();
        driver_wifi_inicializado = false;
    }
    if (interface_estacao != nullptr) {
        esp_netif_destroy_default_wifi(interface_estacao);
        interface_estacao = nullptr;
    }
    if (eventos_wifi != nullptr) {
        vEventGroupDelete(eventos_wifi);
        eventos_wifi = nullptr;
    }
    if (mutex_operacao != nullptr) {
        vSemaphoreDelete(mutex_operacao);
        mutex_operacao = nullptr;
    }
}

void definir_erro(esp_err_t erro) {
    portENTER_CRITICAL(&trava_estado);
    estado.ultimo_erro = erro;
    portEXIT_CRITICAL(&trava_estado);
}

void definir_desconexao_planejada(bool planejada) {
    portENTER_CRITICAL(&trava_estado);
    desconexao_planejada = planejada;
    portEXIT_CRITICAL(&trava_estado);
}

void tratar_evento(void*, esp_event_base_t base, int32_t identificador, void* dados) {
    if (base == WIFI_EVENT && identificador == WIFI_EVENT_STA_START) {
        portENTER_CRITICAL(&trava_estado);
        estado.ativo = true;
        portEXIT_CRITICAL(&trava_estado);
        xEventGroupSetBits(eventos_wifi, BIT_ATIVO);
        return;
    }

    if (base == WIFI_EVENT && identificador == WIFI_EVENT_STA_STOP) {
        portENTER_CRITICAL(&trava_estado);
        estado.ativo = false;
        estado.conectado = false;
        estado.rssi_dbm = 0;
        estado.canal = 0;
        portEXIT_CRITICAL(&trava_estado);
        xEventGroupClearBits(eventos_wifi, BIT_ATIVO | BIT_CONECTADO);
        xEventGroupSetBits(eventos_wifi, BIT_DESCONECTADO);
        return;
    }

    if (base == WIFI_EVENT && identificador == WIFI_EVENT_STA_DISCONNECTED) {
        const auto* evento = static_cast<const wifi_event_sta_disconnected_t*>(dados);
        portENTER_CRITICAL(&trava_estado);
        const bool planejada = desconexao_planejada;
        estado.conectado = false;
        estado.rssi_dbm = 0;
        estado.canal = 0;
        if (evento != nullptr) estado.ultimo_motivo_desconexao = evento->reason;
        if (!planejada) {
            estado.desconexoes++;
            estado.ultimo_erro = ESP_ERR_WIFI_NOT_CONNECT;
        }
        portEXIT_CRITICAL(&trava_estado);

        xEventGroupClearBits(eventos_wifi, BIT_CONECTADO);
        xEventGroupSetBits(eventos_wifi, BIT_DESCONECTADO);
        if (!planejada) {
            xEventGroupSetBits(eventos_wifi, BIT_FALHA);
            ESP_LOGW(ETIQUETA, "Conexão encerrada pelo ponto de acesso (motivo=%d)",
                     evento == nullptr ? 0 : evento->reason);
        }
        return;
    }

    if (base == IP_EVENT && identificador == IP_EVENT_STA_GOT_IP) {
        const auto* evento_ip = static_cast<const ip_event_got_ip_t*>(dados);
        wifi_ap_record_t ponto_acesso{};
        const bool sinal_disponivel = esp_wifi_sta_get_ap_info(&ponto_acesso) == ESP_OK;

        portENTER_CRITICAL(&trava_estado);
        estado.conectado = true;
        estado.conexoes_bem_sucedidas++;
        estado.ultima_conexao_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000);
        estado.ultimo_erro = ESP_OK;
        if (sinal_disponivel) {
            estado.rssi_dbm = ponto_acesso.rssi;
            estado.canal = ponto_acesso.primary;
        }
        portEXIT_CRITICAL(&trava_estado);

        xEventGroupClearBits(eventos_wifi, BIT_FALHA | BIT_DESCONECTADO);
        xEventGroupSetBits(eventos_wifi, BIT_CONECTADO);
        const EstadoWifi copia = gerenciador_wifi_obter_estado();
        ESP_LOGI(ETIQUETA, "Conectado à rede '%s'; IP=" IPSTR
                           "; RSSI=%d dBm; canal=%u",
                 copia.ssid_atual, IP2STR(&evento_ip->ip_info.ip), copia.rssi_dbm,
                 static_cast<unsigned>(copia.canal));
    }
}

wifi_config_t criar_configuracao(const char* ssid, const char* senha) {
    wifi_config_t parametros{};
    std::strncpy(reinterpret_cast<char*>(parametros.sta.ssid), ssid,
                 sizeof(parametros.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(parametros.sta.password), senha,
                 sizeof(parametros.sta.password) - 1);
    parametros.sta.threshold.authmode = senha[0] == '\0' ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    parametros.sta.pmf_cfg.capable = true;
    parametros.sta.pmf_cfg.required = false;
    // Em modo estação, este campo apenas define por qual canal a busca começa.
    // O canal final continua sendo escolhido pelo ponto de acesso.
    parametros.sta.channel = configuracao::CANAL_WIFI_PREFERENCIAL;
    return parametros;
}

esp_err_t aplicar_configuracao_radio() {
    const wifi_bandwidth_t largura =
        configuracao::LARGURA_CANAL_WIFI_MHZ == 40 ? WIFI_BW_HT40
                                                    : WIFI_BW_HT20;
    esp_err_t erro = esp_wifi_set_bandwidth(WIFI_IF_STA, largura);
    if (erro != ESP_OK) return erro;
    erro = esp_wifi_set_max_tx_power(
        static_cast<int8_t>(configuracao::POTENCIA_MAXIMA_WIFI_DBM * 4));
    if (erro != ESP_OK) return erro;

    int8_t potencia_aplicada = 0;
    erro = esp_wifi_get_max_tx_power(&potencia_aplicada);
    if (erro != ESP_OK) return erro;
    ESP_LOGI(ETIQUETA,
             "Rádio STA: canal preferencial=%u, largura=%u MHz, "
             "potência máxima=%d.%02d dBm",
             static_cast<unsigned>(configuracao::CANAL_WIFI_PREFERENCIAL),
             static_cast<unsigned>(configuracao::LARGURA_CANAL_WIFI_MHZ),
             potencia_aplicada / 4, (potencia_aplicada % 4) * 25);
    return ESP_OK;
}

esp_err_t ativar_radio(TickType_t tempo_limite) {
    const EstadoWifi atual = gerenciador_wifi_obter_estado();
    if (atual.ativo) return ESP_OK;

    xEventGroupClearBits(eventos_wifi, BIT_ATIVO);
    esp_err_t erro = esp_wifi_start();
    if (erro != ESP_OK) return erro;

    const TickType_t espera = std::min(tempo_limite, TEMPO_ATIVACAO);
    const EventBits_t bits = xEventGroupWaitBits(
        eventos_wifi, BIT_ATIVO, pdFALSE, pdFALSE, espera);
    if ((bits & BIT_ATIVO) == 0) return ESP_ERR_TIMEOUT;
    erro = aplicar_configuracao_radio();
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Falha ao configurar o rádio Wi-Fi: %s",
                 esp_err_to_name(erro));
        (void)esp_wifi_stop();
    }
    return erro;
}

void cancelar_associacao() {
    definir_desconexao_planejada(true);
    xEventGroupClearBits(eventos_wifi, BIT_DESCONECTADO);
    const esp_err_t erro = esp_wifi_disconnect();
    if (erro == ESP_OK) {
        xEventGroupWaitBits(eventos_wifi, BIT_DESCONECTADO, pdTRUE, pdFALSE,
                            pdMS_TO_TICKS(1000));
    }
    definir_desconexao_planejada(false);
}
}  // namespace

bool gerenciador_wifi_credenciais_validas(const char* ssid, const char* senha) {
    if (ssid == nullptr || senha == nullptr) return false;
    const size_t tamanho_ssid = std::strlen(ssid);
    const size_t tamanho_senha = std::strlen(senha);
    return tamanho_ssid >= 1 && tamanho_ssid <= 32 &&
           (tamanho_senha == 0 || (tamanho_senha >= 8 && tamanho_senha <= 63));
}

esp_err_t gerenciador_wifi_iniciar(const char* ssid, const char* senha) {
    if (!gerenciador_wifi_credenciais_validas(ssid, senha)) {
        ESP_LOGE(ETIQUETA, "Credenciais Wi-Fi fora dos limites permitidos");
        return ESP_ERR_INVALID_ARG;
    }
    if (gerenciador_wifi_obter_estado().inicializado) return ESP_OK;

    esp_err_t erro = esp_netif_init();
    if (erro != ESP_OK && erro != ESP_ERR_INVALID_STATE) return erro;
    erro = esp_event_loop_create_default();
    if (erro != ESP_OK && erro != ESP_ERR_INVALID_STATE) return erro;
    interface_estacao = esp_netif_create_default_wifi_sta();
    if (interface_estacao == nullptr) return ESP_ERR_NO_MEM;

    eventos_wifi = xEventGroupCreate();
    mutex_operacao = xSemaphoreCreateMutex();
    if (eventos_wifi == nullptr || mutex_operacao == nullptr) {
        liberar_inicializacao_parcial();
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t configuracao_wifi = WIFI_INIT_CONFIG_DEFAULT();
    erro = esp_wifi_init(&configuracao_wifi);
    if (erro != ESP_OK) {
        liberar_inicializacao_parcial();
        return erro;
    }
    driver_wifi_inicializado = true;
    erro = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                &tratar_evento, nullptr, &tratador_wifi);
    if (erro == ESP_OK) {
        erro = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                    &tratar_evento, nullptr, &tratador_ip);
    }
    if (erro != ESP_OK) {
        liberar_inicializacao_parcial();
        return erro;
    }

    wifi_config_t parametros = criar_configuracao(ssid, senha);
    erro = esp_wifi_set_mode(WIFI_MODE_STA);
    if (erro == ESP_OK) erro = esp_wifi_set_config(WIFI_IF_STA, &parametros);
    if (erro != ESP_OK) {
        definir_erro(erro);
        liberar_inicializacao_parcial();
        return erro;
    }

    portENTER_CRITICAL(&trava_estado);
    estado.inicializado = true;
    estado.ultimo_erro = ESP_OK;
    std::strncpy(estado.ssid_atual, ssid, sizeof(estado.ssid_atual) - 1);
    portEXIT_CRITICAL(&trava_estado);
    ESP_LOGI(ETIQUETA,
             "Driver Wi-Fi pronto; rádio permanecerá desligado até uma sessão "
             "de rede (canal preferencial=%u, largura=%u MHz, potência=%d dBm)",
             static_cast<unsigned>(configuracao::CANAL_WIFI_PREFERENCIAL),
             static_cast<unsigned>(configuracao::LARGURA_CANAL_WIFI_MHZ),
             static_cast<int>(configuracao::POTENCIA_MAXIMA_WIFI_DBM));
    return ESP_OK;
}

esp_err_t gerenciador_wifi_conectar(const char* ssid, const char* senha,
                                    TickType_t tempo_limite) {
    if (!gerenciador_wifi_credenciais_validas(ssid, senha) || tempo_limite == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!gerenciador_wifi_obter_estado().inicializado || eventos_wifi == nullptr ||
        mutex_operacao == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(mutex_operacao, tempo_limite) != pdTRUE) return ESP_ERR_TIMEOUT;

    esp_err_t erro = ativar_radio(tempo_limite);
    if (erro == ESP_OK) {
        cancelar_associacao();
        wifi_config_t parametros = criar_configuracao(ssid, senha);
        erro = esp_wifi_set_config(WIFI_IF_STA, &parametros);
    }

    if (erro == ESP_OK) {
        portENTER_CRITICAL(&trava_estado);
        estado.tentativas_conexao++;
        estado.conectado = false;
        std::memset(estado.ssid_atual, 0, sizeof(estado.ssid_atual));
        std::strncpy(estado.ssid_atual, ssid, sizeof(estado.ssid_atual) - 1);
        portEXIT_CRITICAL(&trava_estado);
        xEventGroupClearBits(eventos_wifi, BIT_CONECTADO | BIT_FALHA | BIT_DESCONECTADO);
        erro = esp_wifi_connect();
    }

    if (erro == ESP_OK) {
        const EventBits_t bits = xEventGroupWaitBits(
            eventos_wifi, BIT_CONECTADO | BIT_FALHA, pdFALSE, pdFALSE, tempo_limite);
        if ((bits & BIT_CONECTADO) == 0) erro = ESP_ERR_TIMEOUT;
    }
    if (erro != ESP_OK) {
        definir_erro(erro);
        cancelar_associacao();
    }
    xSemaphoreGive(mutex_operacao);
    return erro;
}

esp_err_t gerenciador_wifi_desativar(TickType_t tempo_limite) {
    if (!gerenciador_wifi_obter_estado().inicializado || mutex_operacao == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(mutex_operacao, tempo_limite) != pdTRUE) return ESP_ERR_TIMEOUT;

    const EstadoWifi atual = gerenciador_wifi_obter_estado();
    if (!atual.ativo) {
        xSemaphoreGive(mutex_operacao);
        return ESP_OK;
    }

    cancelar_associacao();
    esp_err_t erro = esp_wifi_stop();
    if (erro == ESP_OK) {
        xEventGroupWaitBits(eventos_wifi, BIT_DESCONECTADO, pdTRUE, pdFALSE,
                            std::min(tempo_limite, pdMS_TO_TICKS(1500)));
    } else {
        definir_erro(erro);
    }
    xSemaphoreGive(mutex_operacao);
    return erro;
}

esp_err_t gerenciador_wifi_atualizar_sinal() {
    const EstadoWifi atual = gerenciador_wifi_obter_estado();
    if (!atual.conectado) return ESP_ERR_INVALID_STATE;
    wifi_ap_record_t ponto_acesso{};
    const esp_err_t erro = esp_wifi_sta_get_ap_info(&ponto_acesso);
    if (erro != ESP_OK) {
        definir_erro(erro);
        return erro;
    }
    portENTER_CRITICAL(&trava_estado);
    estado.rssi_dbm = ponto_acesso.rssi;
    estado.canal = ponto_acesso.primary;
    portEXIT_CRITICAL(&trava_estado);
    return ESP_OK;
}

bool gerenciador_wifi_esta_conectado() {
    return gerenciador_wifi_obter_estado().conectado;
}

EstadoWifi gerenciador_wifi_obter_estado() {
    portENTER_CRITICAL(&trava_estado);
    const EstadoWifi copia = estado;
    portEXIT_CRITICAL(&trava_estado);
    return copia;
}
