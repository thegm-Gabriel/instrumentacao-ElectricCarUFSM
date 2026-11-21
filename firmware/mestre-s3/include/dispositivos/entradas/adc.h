// Declara a leitura calibrada do canal ADC de tensão.
#pragma once

#include <cstdint>

#include "esp_err.h"

esp_err_t leitor_tensao_iniciar();
esp_err_t leitor_tensao_ler(uint16_t* valor_bruto);
// Usa a mesma unidade ADC1; retorna a tensão calibrada do pedal no GPIO3.
esp_err_t leitor_pedal_ler_mv(uint16_t* milivolts);
