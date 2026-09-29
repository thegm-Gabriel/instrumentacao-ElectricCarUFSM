#pragma once

#include <cstdint>

#include "esp_err.h"
#include "servicos/servico_sinalizacao.h"

struct SituacaoPainelLocal {
    bool iniciado = false;
    bool tela_disponivel = false;
    EstadoSinalizacao ultimo_estado_exibido = EstadoSinalizacao::Desligado;
    uint32_t atualizacoes_ok = 0;
    uint32_t falhas_atualizacao = 0;
    esp_err_t ultimo_erro = ESP_OK;
};

esp_err_t servico_painel_local_iniciar();
SituacaoPainelLocal servico_painel_local_obter_situacao();
