#include "servicos/servico_controle_sensores.h"

#include "freertos/FreeRTOS.h"

namespace {
constexpr uint32_t TODOS = (1u << 6) - 1u;
uint32_t sensores_ativos = TODOS;
portMUX_TYPE trava = portMUX_INITIALIZER_UNLOCKED;
}

uint32_t servico_controle_sensores_mascara() {
    portENTER_CRITICAL(&trava);
    const uint32_t copia = sensores_ativos;
    portEXIT_CRITICAL(&trava);
    return copia;
}

bool servico_controle_sensores_ativo(SensorControlado sensor) {
    return (servico_controle_sensores_mascara() &
            (1u << static_cast<uint8_t>(sensor))) != 0;
}

void servico_controle_sensores_definir(SensorControlado sensor, bool ativo) {
    const uint32_t bit = 1u << static_cast<uint8_t>(sensor);
    portENTER_CRITICAL(&trava);
    if (ativo) sensores_ativos |= bit;
    else sensores_ativos &= ~bit;
    portEXIT_CRITICAL(&trava);
}

void servico_controle_sensores_definir_todos(bool ativos) {
    portENTER_CRITICAL(&trava);
    sensores_ativos = ativos ? TODOS : 0u;
    portEXIT_CRITICAL(&trava);
}
