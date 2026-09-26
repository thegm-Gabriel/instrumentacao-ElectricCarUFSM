#include "rede_wifi.h"

#include <string.h>

#include "configuracao.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_visitantes";

static void ao_evento_wifi(void *argumento, esp_event_base_t base, int32_t id, void *dados)
{
    (void)argumento;
    (void)base;
    (void)dados;
    if (id == WIFI_EVENT_AP_STACONNECTED) ESP_LOGI(TAG, "Visitante conectado ao painel");
    if (id == WIFI_EVENT_AP_STADISCONNECTED) ESP_LOGI(TAG, "Visitante desconectado do painel");
}

esp_err_t rede_wifi_iniciar(void)
{
    esp_err_t erro = nvs_flash_init();
    if (erro == ESP_ERR_NVS_NO_FREE_PAGES || erro == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        erro = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(erro, TAG, "NVS");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    esp_netif_t *interface_ap = esp_netif_create_default_wifi_ap();
    if (interface_ap == NULL) return ESP_FAIL;

    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_stop(interface_ap), TAG, "parar DHCP");
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(interface_ap, &ip_info), TAG, "IP do AP");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(interface_ap), TAG, "iniciar DHCP");

    wifi_init_config_t configuracao_inicial = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&configuracao_inicial), TAG, "inicializar Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, ao_evento_wifi, NULL, NULL), TAG, "evento Wi-Fi");
    wifi_config_t configuracao_ap = {0};
    strncpy((char *)configuracao_ap.ap.ssid, WIFI_SSID, sizeof(configuracao_ap.ap.ssid));
    strncpy((char *)configuracao_ap.ap.password, WIFI_SENHA, sizeof(configuracao_ap.ap.password));
    configuracao_ap.ap.ssid_len = strlen(WIFI_SSID);
    configuracao_ap.ap.channel = WIFI_CANAL;
    configuracao_ap.ap.max_connection = WIFI_MAXIMO_CLIENTES;
    configuracao_ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "modo AP");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &configuracao_ap), TAG, "configurar AP");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "iniciar AP");
    ESP_LOGI(TAG, "AP '%s' ativo em " IPSTR, WIFI_SSID, IP2STR(&ip_info.ip));
    return ESP_OK;
}
