#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"
#include "esp_err.h"

struct ConfiguracaoCartaoSd {
    const char* ponto_montagem = nullptr;
    gpio_num_t pino_cs = GPIO_NUM_NC;
    uint32_t frequencia_hz = 10000000;
};

esp_err_t gerenciador_cartao_sd_iniciar(const ConfiguracaoCartaoSd& configuracao);
esp_err_t gerenciador_cartao_sd_salvar(const char* caminho_relativo, const void* dados,
                                       size_t tamanho, bool acrescentar = false);
esp_err_t gerenciador_cartao_sd_finalizar();
bool gerenciador_cartao_sd_esta_montado();
