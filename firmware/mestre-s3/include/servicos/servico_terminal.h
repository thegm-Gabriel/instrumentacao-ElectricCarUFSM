#pragma once

#include <cstdint>

#include "esp_err.h"
#include "servicos/servico_relatorios.h"

// Nome antigo preservado para não quebrar integrações existentes.
using GrupoRelatorioTerminal = GrupoRelatorio;

// Inicia o menu numérico na UART de console.
esp_err_t servico_terminal_iniciar();

// Compatibilidade: a implementação e o estado pertencem a servico_relatorios.
void servico_terminal_solicitar_resumo();

// Retorna true somente quando chegou o momento de exibir o grupo solicitado.
// Esta função controla apenas os logs; não altera a frequência dos periféricos.
bool servico_terminal_deve_exibir(GrupoRelatorioTerminal grupo);
