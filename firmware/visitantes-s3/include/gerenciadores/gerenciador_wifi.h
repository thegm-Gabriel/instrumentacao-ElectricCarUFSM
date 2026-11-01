// Declara o gerenciamento da rede Wi-Fi do satélite dos visitantes.
#pragma once

#include <stdint.h>
#include "esp_err.h"

#define WIFI_AP_CLIENTES_CAPACIDADE 10
typedef struct {
    uint8_t mac[6];
    uint16_t aid;
    uint32_t conectado_ms;
} cliente_wifi_ap_t;

typedef struct {
    uint16_t limite_sessao_minutos;
    uint8_t quantidade;
    cliente_wifi_ap_t clientes[WIFI_AP_CLIENTES_CAPACIDADE];
} situacao_clientes_wifi_t;

esp_err_t gerenciador_wifi_iniciar(void);
esp_err_t gerenciador_wifi_obter_clientes(situacao_clientes_wifi_t *saida);
esp_err_t gerenciador_wifi_definir_limite_sessao(uint16_t minutos);
esp_err_t gerenciador_wifi_desconectar_cliente(const uint8_t mac[6]);
