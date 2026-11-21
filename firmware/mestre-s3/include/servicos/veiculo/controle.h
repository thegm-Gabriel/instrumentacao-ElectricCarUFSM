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
// Conclui a partida; o controle arma quando as verificações forem satisfeitas.
void servico_controle_remoto_concluir_partida();
// Bloqueia rearme e solicita zero ao DAC antes de iniciar qualquer gravação OTA.
esp_err_t servico_controle_remoto_preparar_ota();
// A instalação terminou sem reiniciar o mestre; a parada continua até rearme.
void servico_controle_remoto_encerrar_ota();
esp_err_t servico_controle_remoto_definir(AjusteControleRemoto ajuste, uint32_t valor);
estado_controle_remoto_t servico_controle_remoto_obter_estado();
// Entrada local futura: não é exposta por UART ou HTTP.
esp_err_t servico_controle_remoto_informar_pedal(uint16_t decimos_percentual);
