// Declara a geração de trajetos simulados variados para teste sem sensores.
#pragma once

#include <cstdint>

#include "nucleo/dados_telemetria_veiculo.h"
#include "nucleo/configuracao/veiculo.h"
#include "esp_err.h"

esp_err_t servico_simulador_telemetria_iniciar(
    uint16_t dinamica_percentual = configuracao::DINAMICA_SIMULACAO_PERCENTUAL);
esp_err_t servico_simulador_telemetria_gerar(DadosTelemetriaVeiculo* dados);
esp_err_t servico_simulador_telemetria_definir_ativo(bool ativo);
bool servico_simulador_telemetria_esta_ativo();
uint32_t servico_simulador_telemetria_amostras_geradas();
uint16_t servico_simulador_telemetria_dinamica_percentual();
