#pragma once

#include <cstdint>

#include "dados_telemetria_veiculo.h"
#include "esp_err.h"

struct EstadoSimuladorTelemetria {
    bool iniciado = false;
    uint32_t amostras_geradas = 0;
    uint16_t dinamica_percentual = 100;
    float tempo_simulado_s = 0.0f;
    float percurso_km = 0.0f;
};

esp_err_t servico_simulador_telemetria_iniciar(uint16_t dinamica_percentual = 100);
esp_err_t servico_simulador_telemetria_gerar(DadosTelemetriaVeiculo* dados);
esp_err_t servico_simulador_telemetria_definir_dinamica(uint16_t percentual);
EstadoSimuladorTelemetria servico_simulador_telemetria_obter_estado();
