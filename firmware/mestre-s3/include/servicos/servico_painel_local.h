// Declara a apresentação de estado, inicialização e mensagens no painel local.
#pragma once

#include <cstdint>

#include "esp_err.h"

enum class TipoMensagemPainelLocal : uint8_t {
    Informacao,
    Sucesso,
    Aviso,
    Erro,
};

esp_err_t servico_painel_local_iniciar();
esp_err_t servico_painel_local_atualizar_inicializacao(
    const char* etapa, uint8_t percentual, esp_err_t resultado = ESP_OK);
esp_err_t servico_painel_local_notificar(TipoMensagemPainelLocal tipo,
                                         const char* titulo,
                                         const char* detalhe,
                                         uint32_t duracao_ms = 5000);
