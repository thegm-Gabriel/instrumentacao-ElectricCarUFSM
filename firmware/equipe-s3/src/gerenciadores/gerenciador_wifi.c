// Cria o SoftAP da equipe com a configuração padrão de rede do ESP-IDF.
#include "gerenciadores/gerenciador_wifi.h"

#include <string.h>

#include "nucleo/configuracao.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "rede_wifi";
static portMUX_TYPE trava_clientes = portMUX_INITIALIZER_UNLOCKED;
static struct {
    uint8_t mac[6];
    uint16_t aid;
    int64_t entrada_us;
    bool expirando;
} sessoes[WIFI_AP_CLIENTES_CAPACIDADE];
static uint16_t limite_sessao_minutos = 15;

static void registrar_entrada(const uint8_t mac[6], uint16_t aid)
{
    portENTER_CRITICAL(&trava_clientes);
    unsigned indice = WIFI_AP_CLIENTES_CAPACIDADE;
    for (unsigned i = 0; i < WIFI_AP_CLIENTES_CAPACIDADE; ++i) {
        if (sessoes[i].aid == aid && memcmp(sessoes[i].mac, mac, 6) == 0) { indice = i; break; }
        if (indice == WIFI_AP_CLIENTES_CAPACIDADE && sessoes[i].aid == 0) indice = i;
    }
    if (indice < WIFI_AP_CLIENTES_CAPACIDADE) {
        memcpy(sessoes[indice].mac, mac, 6);
        sessoes[indice].aid = aid;
        sessoes[indice].entrada_us = esp_timer_get_time();
        sessoes[indice].expirando = false;
    }
    portEXIT_CRITICAL(&trava_clientes);
}

static void registrar_saida(const uint8_t mac[6], uint16_t aid)
{
    portENTER_CRITICAL(&trava_clientes);
    for (unsigned i = 0; i < WIFI_AP_CLIENTES_CAPACIDADE; ++i) {
        if (sessoes[i].aid == aid && memcmp(sessoes[i].mac, mac, 6) == 0) {
            sessoes[i].aid = 0;
            break;
        }
    }
    portEXIT_CRITICAL(&trava_clientes);
}

static void monitorar_sessoes(void *argumento)
{
    (void)argumento;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        const int64_t agora = esp_timer_get_time();
        uint16_t expulsar[WIFI_AP_CLIENTES_CAPACIDADE];
        unsigned quantidade = 0;
        portENTER_CRITICAL(&trava_clientes);
        const uint16_t limite = limite_sessao_minutos;
        if (limite != 0) for (unsigned i = 0; i < WIFI_AP_CLIENTES_CAPACIDADE; ++i) {
            if (sessoes[i].aid && !sessoes[i].expirando &&
                agora - sessoes[i].entrada_us >= (int64_t)limite * 60000000LL) {
                sessoes[i].expirando = true;
                expulsar[quantidade++] = sessoes[i].aid;
            }
        }
        portEXIT_CRITICAL(&trava_clientes);
        for (unsigned i = 0; i < quantidade; ++i) {
            const esp_err_t erro = esp_wifi_deauth_sta(expulsar[i]);
            ESP_LOGW(TAG, "Limite de sessão: AID=%u, %u min, resultado=%s",
                     (unsigned)expulsar[i], (unsigned)limite, esp_err_to_name(erro));
            if (erro != ESP_OK) {
                portENTER_CRITICAL(&trava_clientes);
                for (unsigned j = 0; j < WIFI_AP_CLIENTES_CAPACIDADE; ++j)
                    if (sessoes[j].aid == expulsar[i]) sessoes[j].expirando = false;
                portEXIT_CRITICAL(&trava_clientes);
            }
        }
    }
}

esp_err_t gerenciador_wifi_obter_clientes(situacao_clientes_wifi_t *saida)
{
    if (!saida) return ESP_ERR_INVALID_ARG;
    memset(saida, 0, sizeof(*saida));
    wifi_sta_list_t lista = {0};
    const esp_err_t erro = esp_wifi_ap_get_sta_list(&lista);
    if (erro != ESP_OK) return erro;
    const int64_t agora = esp_timer_get_time();
    portENTER_CRITICAL(&trava_clientes);
    saida->limite_sessao_minutos = limite_sessao_minutos;
    for (int i = 0; i < lista.num && saida->quantidade < WIFI_AP_CLIENTES_CAPACIDADE; ++i) {
        cliente_wifi_ap_t *cliente = &saida->clientes[saida->quantidade++];
        memcpy(cliente->mac, lista.sta[i].mac, 6);
        for (unsigned j = 0; j < WIFI_AP_CLIENTES_CAPACIDADE; ++j) {
            if (sessoes[j].aid && memcmp(sessoes[j].mac, cliente->mac, 6) == 0) {
                cliente->aid = sessoes[j].aid;
                cliente->conectado_ms = (uint32_t)((agora - sessoes[j].entrada_us) / 1000);
                break;
            }
        }
    }
    portEXIT_CRITICAL(&trava_clientes);
    return ESP_OK;
}

esp_err_t gerenciador_wifi_definir_limite_sessao(uint16_t minutos)
{
    if (minutos > 1440) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    esp_err_t erro = nvs_open("wifi_sessao", NVS_READWRITE, &nvs);
    if (erro != ESP_OK) return erro;
    erro = nvs_set_u16(nvs, "limite_min", minutos);
    if (erro == ESP_OK) erro = nvs_commit(nvs);
    nvs_close(nvs);
    if (erro == ESP_OK) {
        portENTER_CRITICAL(&trava_clientes);
        limite_sessao_minutos = minutos;
        portEXIT_CRITICAL(&trava_clientes);
        ESP_LOGI(TAG, "Limite de sessão Wi-Fi: %u min", (unsigned)minutos);
    }
    return erro;
}

esp_err_t gerenciador_wifi_desconectar_cliente(const uint8_t mac[6])
{
    if (!mac) return ESP_ERR_INVALID_ARG;
    uint16_t aid = 0;
    esp_err_t erro = esp_wifi_ap_get_sta_aid(mac, &aid);
    if (erro != ESP_OK) return erro;
    erro = esp_wifi_deauth_sta(aid);
    ESP_LOGW(TAG, "Desconexão manual: cliente " MACSTR " AID=%u resultado=%s",
             MAC2STR(mac), (unsigned)aid, esp_err_to_name(erro));
    return erro;
}

_Static_assert(sizeof(WIFI_SSID) - 1 <= 32, "SSID do SoftAP muito longo");
_Static_assert(sizeof(WIFI_SENHA) - 1 >= 8 &&
               sizeof(WIFI_SENHA) - 1 <= 63, "Senha WPA2 inválida");

static void ao_evento_wifi(void *argumento, esp_event_base_t base,
                            int32_t id, void *dados)
{
    (void)argumento;
    (void)base;
    if (id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "SoftAP iniciado");
    } else if (id == WIFI_EVENT_AP_STOP) {
        ESP_LOGW(TAG, "SoftAP parou");
    } else if (id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *cliente = dados;
        registrar_entrada(cliente->mac, cliente->aid);
        ESP_LOGI(TAG, "Cliente " MACSTR " associado, AID=%u",
                 MAC2STR(cliente->mac), (unsigned)cliente->aid);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *cliente = dados;
        registrar_saida(cliente->mac, cliente->aid);
        ESP_LOGI(TAG, "Cliente " MACSTR " saiu, AID=%u motivo=%u",
                 MAC2STR(cliente->mac), (unsigned)cliente->aid,
                 (unsigned)cliente->reason);
    }
}

static void ao_ip_cliente(void *argumento, esp_event_base_t base,
                          int32_t id, void *dados)
{
    (void)argumento;
    (void)base;
    (void)id;
    const ip_event_ap_staipassigned_t *cliente = dados;
    ESP_LOGI(TAG, "DHCP entregou IP " IPSTR, IP2STR(&cliente->ip));
}

// Anuncia a entrada do painel ao sistema do cliente, mantendo o DHCP ativo
// mesmo se a opção de portal não estiver disponível nesta versão do ESP-IDF.
static esp_err_t anunciar_portal(esp_netif_t *interface_ap)
{
    esp_err_t erro = esp_netif_dhcps_stop(interface_ap);
    if (erro != ESP_OK && erro != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "Portal não anunciado; DHCP mantido: %s",
                 esp_err_to_name(erro));
        return ESP_OK;
    }

    uint8_t oferecer_dns = 0;
    const esp_err_t erro_dns = esp_netif_dhcps_option(
        interface_ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
        &oferecer_dns, sizeof(oferecer_dns));
    const esp_err_t erro_opcao = esp_netif_dhcps_option(
        interface_ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
        (void *)URL_IP_PORTAL_ENTRADA, strlen(URL_IP_PORTAL_ENTRADA));
    erro = esp_netif_dhcps_start(interface_ap);
    ESP_RETURN_ON_ERROR(erro, TAG, "reiniciar DHCP do SoftAP");

    if (erro_dns != ESP_OK) {
        ESP_LOGW(TAG, "Não foi possível omitir DNS do DHCP: %s",
                 esp_err_to_name(erro_dns));
    }
    if (erro_opcao == ESP_OK) {
        ESP_LOGI(TAG, "Portal anunciado pelo DHCP: %s", URL_IP_PORTAL_ENTRADA);
    } else {
        ESP_LOGW(TAG, "Opção 114 indisponível; acesso por IP ativo: %s",
                 esp_err_to_name(erro_opcao));
    }
    return ESP_OK;
}

esp_err_t gerenciador_wifi_iniciar(void)
{
    esp_err_t erro = nvs_flash_init();
    if (erro == ESP_ERR_NVS_NO_FREE_PAGES || erro == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "limpar NVS incompatível");
        erro = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(erro, TAG, "inicializar NVS");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "inicializar rede");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");

    esp_netif_t *interface_ap = esp_netif_create_default_wifi_ap();
    if (interface_ap == NULL) return ESP_FAIL;

    wifi_init_config_t inicializacao = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&inicializacao), TAG, "inicializar rádio");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, ao_evento_wifi, NULL, NULL),
        TAG, "registrar eventos Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, ao_ip_cliente, NULL, NULL),
        TAG, "registrar eventos DHCP");

    wifi_config_t configuracao = {0};
    memcpy(configuracao.ap.ssid, WIFI_SSID, sizeof(WIFI_SSID) - 1);
    memcpy(configuracao.ap.password, WIFI_SENHA, sizeof(WIFI_SENHA) - 1);
    configuracao.ap.ssid_len = sizeof(WIFI_SSID) - 1;
    configuracao.ap.channel = WIFI_CANAL;
    configuracao.ap.max_connection = WIFI_MAXIMO_CLIENTES;
    configuracao.ap.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "modo SoftAP");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &configuracao),
                        TAG, "configurar SoftAP");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "iniciar SoftAP");
    nvs_handle_t nvs_sessao;
    if (nvs_open("wifi_sessao", NVS_READONLY, &nvs_sessao) == ESP_OK) {
        uint16_t salvo = 15;
        if (nvs_get_u16(nvs_sessao, "limite_min", &salvo) == ESP_OK && salvo <= 1440)
            limite_sessao_minutos = salvo;
        nvs_close(nvs_sessao);
    }
    if (xTaskCreate(monitorar_sessoes, "wifi_sessoes", 3072, NULL, 4, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(anunciar_portal(interface_ap), TAG, "DHCP do SoftAP");

    esp_netif_ip_info_t ip;
    ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(interface_ap, &ip),
                        TAG, "consultar IP do SoftAP");
    ESP_LOGI(TAG, "Rede '%s' pronta em " IPSTR " (canal=%u, clientes=%u)",
             WIFI_SSID, IP2STR(&ip.ip), (unsigned)WIFI_CANAL,
             (unsigned)WIFI_MAXIMO_CLIENTES);
    return ESP_OK;
}
