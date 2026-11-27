// Controle lógico em RAM; a ligação física dos atuadores é uma etapa separada.
#pragma once
#include "esp_err.h"
#include "controle_remoto.h"

enum class AjusteControleRemoto {
    LimiteMarcha, VelocidadeMaxima,
    AceleradorMaximo, ZonaMortaPedal, TensaoMinimaDac, Parada, Reversao,
    PermissaoReversao,
};
esp_err_t servico_controle_remoto_iniciar();
// Conclui a partida e libera a solicitação de armamento quando o veículo estiver seguro.
void servico_controle_remoto_concluir_partida(bool observando_ota = false);
// Bloqueia rearme e solicita zero ao DAC antes de iniciar qualquer gravação OTA.
esp_err_t servico_controle_remoto_preparar_ota();
// A instalação terminou sem reiniciar o mestre; a parada continua até rearme.
void servico_controle_remoto_encerrar_ota();
esp_err_t servico_controle_remoto_definir(AjusteControleRemoto ajuste, uint32_t valor,
                                         int64_t recebido_em_us = 0, bool somente_desarmado = false);
estado_controle_remoto_t servico_controle_remoto_obter_estado();
// Último percentual confirmado no DAC, em décimos de %, com zero durante corte ou falha.
struct SaidaControleVeiculo {
    uint16_t acelerador_decimos;
    bool rele_energizado;
    bool reversao;
};
SaidaControleVeiculo servico_controle_remoto_obter_saida_aplicada();
