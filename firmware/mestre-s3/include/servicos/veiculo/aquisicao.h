// Coordena o ciclo de aquisição e a disponibilidade dos sensores.
#pragma once

#include <cstdint>

#include "nucleo/dados_telemetria_veiculo.h"
#include "esp_err.h"

enum class SensorControlado : uint8_t {
    Adc,
    Mpu6050,
    Encoders,
    Ina228,
    TemperaturaEsp,
    Quantidade,
};

using RegistradorResultadoSensor = esp_err_t (*)(SensorControlado, esp_err_t);

esp_err_t servico_aquisicao_preparar(RegistradorResultadoSensor registrar_resultado);

// Reúne os estados reais mais recentes, sem disparar leituras dos dispositivos.
DadosTelemetriaVeiculo servico_aquisicao_obter_amostra_real();

bool servico_aquisicao_sensor_ativo(SensorControlado sensor);
uint32_t servico_aquisicao_obter_sensores_ativos();
void servico_aquisicao_definir_sensor(SensorControlado sensor, bool ativo);
void servico_aquisicao_definir_todos_sensores(bool ativos);
