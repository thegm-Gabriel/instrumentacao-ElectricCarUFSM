// Monta o cartão SD em FAT usando SPI e expõe seu estado ao serviço de registro.
#pragma once

#include <cstdint>

#include "driver/gpio.h"
#include "esp_err.h"

struct ConfiguracaoCartaoSd {
    const char* ponto_montagem = nullptr;
    gpio_num_t pino_clk = GPIO_NUM_NC;
    gpio_num_t pino_mosi = GPIO_NUM_NC;
    gpio_num_t pino_miso = GPIO_NUM_NC;
    gpio_num_t pino_cs = GPIO_NUM_NC;
    uint32_t frequencia_hz = 10000000;
    uint8_t maximo_arquivos_abertos = 4;
};

esp_err_t gerenciador_cartao_sd_iniciar(const ConfiguracaoCartaoSd& configuracao);
