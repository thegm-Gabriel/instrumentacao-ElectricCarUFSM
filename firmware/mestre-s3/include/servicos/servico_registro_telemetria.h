// Registra telemetria a 1 Hz no SD e envia os CSVs pendentes ao Google sob demanda.
#pragma once

#include <cstdint>

#include "dados_telemetria_veiculo.h"
#include "esp_err.h"

struct EstadoRegistroTelemetria {
    bool iniciado = false;
    bool cartao_disponivel = false;
    bool sincronizacao_em_andamento = false;
    uint32_t amostras_gravadas = 0;
    uint32_t amostras_descartadas = 0;
    uint32_t arquivos_pendentes = 0;
    uint32_t arquivos_enviados = 0;
    esp_err_t ultimo_erro = ESP_OK;
};

esp_err_t servico_registro_telemetria_iniciar();
esp_err_t servico_registro_telemetria_registrar(
    const DadosTelemetriaVeiculo& dados);
esp_err_t servico_registro_telemetria_solicitar_sincronizacao();
EstadoRegistroTelemetria servico_registro_telemetria_obter_estado();
