#pragma once

#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

struct EstadoWifi {
    bool inicializado = false;
    bool conectado = false;
    uint32_t tentativas_conexao = 0;
    uint32_t desconexoes = 0;
    esp_err_t ultimo_erro = ESP_OK;
};

esp_err_t gerenciador_wifi_iniciar(const char* ssid, const char* senha);
esp_err_t gerenciador_wifi_aguardar_conexao(TickType_t tempo_limite);
esp_err_t gerenciador_wifi_reconectar();
bool gerenciador_wifi_esta_conectado();
EstadoWifi gerenciador_wifi_obter_estado();
