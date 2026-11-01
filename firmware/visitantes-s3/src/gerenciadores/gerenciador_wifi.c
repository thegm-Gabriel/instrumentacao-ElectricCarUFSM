// Mantém o ponto de acesso e o estado da rede Wi-Fi dos visitantes.
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
#include "lwip/inet.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_visitantes";
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



static void ao_evento_wifi(void *argumento, esp_event_base_t base, int32_t id, void *dados)
{
    (void)argumento;
    (void)base;
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *cliente = dados;
        registrar_entrada(cliente->mac, cliente->aid);
        ESP_LOGI(TAG, "Cliente " MACSTR " conectado, AID=%u",
                 MAC2STR(cliente->mac), (unsigned)cliente->aid);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *cliente = dados;
        registrar_saida(cliente->mac, cliente->aid);
        ESP_LOGI(TAG, "Cliente " MACSTR " desconectado, AID=%u motivo=%u",
                 MAC2STR(cliente->mac), (unsigned)cliente->aid,
                 (unsigned)cliente->reason);
    }
}

esp_err_t gerenciador_wifi_iniciar(void)
{
    esp_err_t erro = nvs_flash_init();
    if (erro == ESP_ERR_NVS_NO_FREE_PAGES || erro == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        erro = nvs_flash_erase();
        if (erro != ESP_OK) return erro;
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
    esp_netif_dns_info_t servidor_dns = {0};
    servidor_dns.ip.type = ESP_IPADDR_TYPE_V4;
    servidor_dns.ip.u_addr.ip4 = ip_info.ip;
    esp_err_t erro_opcional = esp_netif_set_dns_info(
        interface_ap, ESP_NETIF_DNS_MAIN, &servidor_dns);
    if (erro_opcional != ESP_OK) {
        ESP_LOGW(TAG, "DNS local indisponível: %s",
                 esp_err_to_name(erro_opcional));
    }
    uint8_t oferecer_dns = 0x02;
    erro_opcional = esp_netif_dhcps_option(
        interface_ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
        &oferecer_dns, sizeof(oferecer_dns));
    if (erro_opcional != ESP_OK) {
        ESP_LOGW(TAG, "DHCP não anunciou o DNS local: %s",
                 esp_err_to_name(erro_opcional));
    }
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
    const wifi_bandwidth_t largura = WIFI_LARGURA_CANAL_MHZ == 40
                                         ? WIFI_BW_HT40 : WIFI_BW_HT20;
    ESP_RETURN_ON_ERROR(esp_wifi_set_bandwidth(WIFI_IF_AP, largura), TAG,
                        "configurar largura do canal");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "iniciar AP");
    nvs_handle_t nvs_sessao;
    if (nvs_open("wifi_sessao", NVS_READONLY, &nvs_sessao) == ESP_OK) {
        uint16_t salvo = 15;
        if (nvs_get_u16(nvs_sessao, "limite_min", &salvo) == ESP_OK && salvo <= 1440)
            limite_sessao_minutos = salvo;
        nvs_close(nvs_sessao);
    }
    if (xTaskCreate(monitorar_sessoes, "wifi_sessoes", 3072, NULL, 4, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    esp_err_t erro_potencia =
        esp_wifi_set_max_tx_power((int8_t)(WIFI_POTENCIA_MAXIMA_DBM * 4));
    if (erro_potencia != ESP_OK) {
        ESP_LOGW(TAG, "Potência solicitada indisponível; mantendo a do driver: %s",
                 esp_err_to_name(erro_potencia));
    }
    int8_t potencia_aplicada = 0;
    erro_potencia = esp_wifi_get_max_tx_power(&potencia_aplicada);
    if (erro_potencia != ESP_OK) {
        ESP_LOGW(TAG, "Não foi possível consultar a potência do rádio: %s",
                 esp_err_to_name(erro_potencia));
    }
    ESP_LOGI(TAG, "AP '%s' ativo em " IPSTR, WIFI_SSID, IP2STR(&ip_info.ip));
    if (erro_potencia == ESP_OK) {
        ESP_LOGI(TAG,
                 "Rádio AP: canal=%u, largura=%u MHz, potência máxima=%d.%02d dBm",
                 (unsigned)WIFI_CANAL, (unsigned)WIFI_LARGURA_CANAL_MHZ,
                 potencia_aplicada / 4, (potencia_aplicada % 4) * 25);
    }
    return ESP_OK;
}
