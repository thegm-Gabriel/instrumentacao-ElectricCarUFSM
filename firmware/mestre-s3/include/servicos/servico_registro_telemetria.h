// Registra telemetria a 1 Hz no SD e envia os CSVs pendentes ao Google sob demanda.
#pragma once

#include <cstdint>

#include "dados_telemetria_veiculo.h"
#include "esp_err.h"

struct EstadoRegistroTelemetria {
    bool iniciado = false;
    bool cartao_disponivel = false;
    bool sincronizacao_em_andamento = false;
    bool aguardando_confirmacao_upload = false;
    uint32_t amostras_gravadas = 0;
    uint32_t amostras_descartadas = 0;
    uint32_t arquivos_pendentes = 0;
    uint32_t arquivos_enviados = 0;
    uint32_t arquivos_upload_total = 0;
    uint32_t arquivos_upload_concluidos = 0;
    uint32_t arquivo_atual_bytes_enviados = 0;
    uint32_t arquivo_atual_bytes_total = 0;
    uint32_t velocidade_upload_bytes_s = 0;
    uint32_t tempo_restante_upload_s = 0;
    uint32_t sincronizacoes_finalizadas = 0;
    uint32_t arquivos_ultima_sincronizacao = 0;
    uint32_t bytes_ultima_sincronizacao = 0;
    uint32_t duracao_ultima_sincronizacao_ms = 0;
    uint32_t fim_ultima_sincronizacao_ms = 0;
    char arquivo_em_upload[96] = {};
    esp_err_t ultimo_resultado_sincronizacao = ESP_OK;
    esp_err_t ultimo_erro = ESP_OK;
};

esp_err_t servico_registro_telemetria_iniciar();
esp_err_t servico_registro_telemetria_registrar(
    const DadosTelemetriaVeiculo& dados);
esp_err_t servico_registro_telemetria_solicitar_sincronizacao();
EstadoRegistroTelemetria servico_registro_telemetria_obter_estado();
