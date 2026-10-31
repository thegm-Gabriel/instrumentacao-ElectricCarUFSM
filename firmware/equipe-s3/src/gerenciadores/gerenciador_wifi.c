// Cria o SoftAP da equipe com a configuração padrão de rede do ESP-IDF.
#include "gerenciadores/gerenciador_wifi.h"

#include <string.h>

#include "nucleo/configuracao.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "rede_wifi";

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
        ESP_LOGI(TAG, "Cliente " MACSTR " associado, AID=%u",
                 MAC2STR(cliente->mac), (unsigned)cliente->aid);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *cliente = dados;
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
    ESP_RETURN_ON_ERROR(anunciar_portal(interface_ap), TAG, "DHCP do SoftAP");

    esp_netif_ip_info_t ip;
    ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(interface_ap, &ip),
                        TAG, "consultar IP do SoftAP");
    ESP_LOGI(TAG, "Rede '%s' pronta em " IPSTR " (canal=%u, clientes=%u)",
             WIFI_SSID, IP2STR(&ip.ip), (unsigned)WIFI_CANAL,
             (unsigned)WIFI_MAXIMO_CLIENTES);
    return ESP_OK;
}
