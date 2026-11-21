// Declara o dispositivo lógico de encoder incremental de cada roda.
#pragma once

#include <cstdint>

#include "driver/gpio.h"
#include "esp_err.h"

enum class EncoderIncrementalId : uint8_t {
    RodaEsquerda = 0,
    RodaDireita,
    Quantidade,
};

struct ConfiguracaoEncoderIncremental {
    gpio_num_t pino_a = GPIO_NUM_NC;
    gpio_num_t pino_b = GPIO_NUM_NC;
    uint32_t pulsos_por_giro = 0;
    uint32_t filtro_ruido_ns = 0;
};

struct EstadoEncoderIncremental {
    bool inicializado = false;
    gpio_num_t pino_a = GPIO_NUM_NC;
    gpio_num_t pino_b = GPIO_NUM_NC;
    uint32_t pulsos_por_giro = 0;
    uint32_t contagens_por_giro = 0;
    uint64_t contagem_atual = 0;
    uint32_t transbordamentos_compensados = 0;
    uint32_t leituras_ok = 0;
    uint32_t erros_leitura = 0;
    esp_err_t ultimo_erro = ESP_OK;
};

esp_err_t encoder_incremental_iniciar(
    EncoderIncrementalId id, const ConfiguracaoEncoderIncremental& configuracao);
esp_err_t encoder_incremental_ler(EncoderIncrementalId id, uint64_t* contagem);
EstadoEncoderIncremental encoder_incremental_obter_estado(EncoderIncrementalId id);
const char* encoder_incremental_nome(EncoderIncrementalId id);
