// Leitura calibrada do pedal e última amostra bruta do mesmo canal.
#pragma once

#include <cstdint>

#include "esp_err.h"

esp_err_t leitor_pedal_iniciar();
esp_err_t leitor_pedal_obter_bruto(uint16_t* valor_bruto);
esp_err_t leitor_pedal_ler_mv(uint16_t* milivolts);
