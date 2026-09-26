#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

struct EstatisticasI2c {
    bool inicializado = false;
    uint32_t operacoes_ok = 0;
    uint32_t erros = 0;
    uint64_t bytes_escritos = 0;
    uint64_t bytes_lidos = 0;
    esp_err_t ultimo_erro = ESP_OK;
};

esp_err_t gerenciador_i2c_iniciar();
esp_err_t gerenciador_i2c_escrever(uint8_t endereco, const void* dados, size_t tamanho,
                                   TickType_t tempo_limite = pdMS_TO_TICKS(100));
esp_err_t gerenciador_i2c_escrever_ler(uint8_t endereco, const void* comando,
                                       size_t tamanho_comando, void* resposta,
                                       size_t tamanho_resposta,
                                       TickType_t tempo_limite = pdMS_TO_TICKS(100));
EstatisticasI2c gerenciador_i2c_obter_estatisticas();
