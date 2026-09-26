#pragma once

#include "esp_err.h"
#include "dados_telemetria.h"

esp_err_t receptor_uart_iniciar(void);
void receptor_uart_obter_dados(dados_telemetria_t *dados);
