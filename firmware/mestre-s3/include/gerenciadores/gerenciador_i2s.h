#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

struct ConfiguracaoI2s {
    uint32_t taxa_amostragem_hz = 0;
    uint8_t bits_por_amostra = 0;
    uint8_t quantidade_canais = 0;
};

esp_err_t gerenciador_i2s_iniciar(const ConfiguracaoI2s& configuracao);
esp_err_t gerenciador_i2s_transmitir(const void* amostras, size_t quantidade_bytes,
                                     TickType_t tempo_limite);
bool gerenciador_i2s_esta_pronto();
