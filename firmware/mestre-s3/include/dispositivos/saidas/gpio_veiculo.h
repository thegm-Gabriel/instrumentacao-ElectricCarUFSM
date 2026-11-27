#pragma once

#include "esp_err.h"
#include "controle_remoto.h"

struct SaidasMarcha {
    bool valida;
    bool modo_a;
    bool modo_b;
};

constexpr SaidasMarcha saidas_veiculo_marcha(uint8_t marcha) {
    return {marcha >= FAIXA_MARCHA_L && marcha <= FAIXA_MARCHA_H,
            marcha == FAIXA_MARCHA_L, marcha == FAIXA_MARCHA_H};
}

// Todas as saídas começam desligadas. O relé usa o contato normalmente aberto.
esp_err_t saidas_veiculo_iniciar();
esp_err_t saidas_veiculo_aplicar(const estado_controle_remoto_t& estado, bool& rele_habilitado);
esp_err_t saidas_veiculo_cortar_por_parada(uint32_t revisao);
esp_err_t saidas_veiculo_definir_permissao_reversao(bool permitir,
                                                  bool parar, uint32_t revisao);
esp_err_t saidas_veiculo_bloquear_ota();
void saidas_veiculo_encerrar_ota();
void saidas_veiculo_bloquear_falha();
