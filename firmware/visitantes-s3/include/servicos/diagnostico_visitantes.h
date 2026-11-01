#pragma once

#include "esp_err.h"
#include "protocolo_comandos.h"

esp_err_t diagnostico_visitantes_obter(resposta_diagnostico_satelite_t *saida);
