// Declara a geração determinística de telemetria simulada.
#pragma once

#include <cstdint>

#include "dados_telemetria_veiculo.h"
#include "esp_err.h"

esp_err_t servico_simulador_telemetria_iniciar(uint16_t dinamica_percentual = 100);
esp_err_t servico_simulador_telemetria_gerar(DadosTelemetriaVeiculo* dados);
