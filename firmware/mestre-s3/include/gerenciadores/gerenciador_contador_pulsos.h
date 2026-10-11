// Declara o acesso robusto aos contadores de pulsos PCNT.
#pragma once

#include <cstdint>

#include "driver/gpio.h"
#include "esp_err.h"

enum class ContadorPulsosId : uint8_t {
    Primeiro = 0,
    Segundo,
    Quantidade,
};

struct ConfiguracaoContadorPulsos {
    gpio_num_t pino_a = GPIO_NUM_NC;
    gpio_num_t pino_b = GPIO_NUM_NC;
    uint32_t filtro_ruido_ns = 0;
};

struct EstatisticasContadorPulsos {
    bool inicializado = false;
    uint64_t contagem_atual = 0;
    uint32_t transbordamentos_compensados = 0;
    uint32_t leituras_ok = 0;
    uint32_t erros_leitura = 0;
    esp_err_t ultimo_erro = ESP_OK;
};

// Conta todas as bordas dos canais A e B pelo periférico PCNT. Cada pulso
// nominal do encoder gera quatro contagens, independentemente do sentido.
esp_err_t gerenciador_contador_pulsos_iniciar(
    ContadorPulsosId id, const ConfiguracaoContadorPulsos& configuracao);
esp_err_t gerenciador_contador_pulsos_ler(ContadorPulsosId id,
                                          uint64_t* contagem);
EstatisticasContadorPulsos gerenciador_contador_pulsos_obter_estatisticas(
    ContadorPulsosId id);
