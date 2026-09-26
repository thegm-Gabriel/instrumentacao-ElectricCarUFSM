#pragma once

#include <cstdint>

#include "esp_err.h"

enum class EstadoServicoOta : uint8_t {
    Desabilitado,
    Inicializando,
    AguardandoRede,
    Verificando,
    Atualizado,
    AguardandoAutorizacao,
    Baixando,
    Aplicado,
    Cancelado,
    Falha,
};

struct SituacaoOta {
    EstadoServicoOta estado = EstadoServicoOta::Desabilitado;
    char versao_atual[32] = {};
    char versao_disponivel[32] = {};
    uint32_t verificacoes = 0;
    uint32_t falhas = 0;
    esp_err_t ultimo_erro = ESP_OK;
};

esp_err_t servico_ota_iniciar();
esp_err_t servico_ota_solicitar_verificacao();
esp_err_t servico_ota_autorizar_atualizacao();
esp_err_t servico_ota_cancelar_atualizacao();
SituacaoOta servico_ota_obter_situacao();
const char* servico_ota_nome_estado(EstadoServicoOta estado);
