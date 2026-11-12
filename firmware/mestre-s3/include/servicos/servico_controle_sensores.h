// Permite pausar a aquisição física durante diagnósticos pelo terminal.
#pragma once

#include <cstdint>

enum class SensorControlado : uint8_t {
    Adc,
    Mpu6050,
    Encoders,
    Ina228,
    Microfone,
    TemperaturaEsp,
};

bool servico_controle_sensores_ativo(SensorControlado sensor);
void servico_controle_sensores_definir(SensorControlado sensor, bool ativo);
void servico_controle_sensores_definir_todos(bool ativos);
uint32_t servico_controle_sensores_mascara();
