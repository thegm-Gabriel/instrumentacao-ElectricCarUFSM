// Coordena o ciclo de aquisição e a disponibilidade dos sensores.
#pragma once

#include <cstdint>

#include "dados_telemetria_veiculo.h"
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
// O consumidor roda na tarefa de aquisição, com a amostra do ciclo.
using ConsumidorAmostraLocal = void (*)(const DadosTelemetriaVeiculo&);

esp_err_t servico_aquisicao_preparar(RegistradorResultadoSensor registrar_resultado);
esp_err_t servico_aquisicao_iniciar(ConsumidorAmostraLocal consumidor);

// Retorna false até o primeiro ciclo publicar uma amostra local.
bool servico_aquisicao_obter_ultima_amostra(DadosTelemetriaVeiculo& dados);

bool servico_aquisicao_sensor_ativo(SensorControlado sensor);
uint32_t servico_aquisicao_obter_sensores_ativos();
void servico_aquisicao_definir_sensor(SensorControlado sensor, bool ativo);
void servico_aquisicao_definir_todos_sensores(bool ativos);
