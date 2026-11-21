#pragma once

#include "esp_err.h"
#include "controle_remoto.h"

// Todas as saídas começam desligadas. O relé usa o contato normalmente aberto.
esp_err_t saidas_veiculo_iniciar();
esp_err_t saidas_veiculo_aplicar(const estado_controle_remoto_t& estado);
esp_err_t saidas_veiculo_definir_permissao_reversao(bool permitir,
                                                  bool parar, uint32_t revisao);
esp_err_t saidas_veiculo_bloquear_ota();
void saidas_veiculo_encerrar_ota();
void saidas_veiculo_bloquear_falha();
