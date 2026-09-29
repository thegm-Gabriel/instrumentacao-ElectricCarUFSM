#pragma once

#include "esp_err.h"

// Inicializa e supervisiona as UARTs dos satélites. É o único consumidor dos
// retornos e encaminha confirmações, respostas OTA e comandos ao serviço correto.
esp_err_t servico_enlace_satelites_iniciar();
