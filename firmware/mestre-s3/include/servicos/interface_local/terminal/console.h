// Declara o menu serial de operação e manutenção do mestre.
#pragma once

#include <cstdint>

#include "esp_err.h"

constexpr uint8_t QUANTIDADE_FONTES_LOG_TERMINAL = 18;

enum class AjusteLogsTerminal : uint8_t {
    Global,
    ApenasAvisosProjeto,
    Cores,
    Fonte,
};

struct EstadoLogsTerminal {
    uint8_t nivel_global;
    bool apenas_avisos_projeto;
    bool cores;
    uint8_t niveis_fontes[QUANTIDADE_FONTES_LOG_TERMINAL];
};

// Inicia o menu numérico na UART de console.
esp_err_t servico_terminal_iniciar();
esp_err_t servico_terminal_obter_logs(EstadoLogsTerminal* estado);
esp_err_t servico_terminal_definir_logs(AjusteLogsTerminal ajuste, uint8_t alvo,
                                      uint8_t valor);
