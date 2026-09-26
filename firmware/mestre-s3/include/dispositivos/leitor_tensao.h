#pragma once

#include <cstdint>

#include "esp_err.h"

esp_err_t leitor_tensao_iniciar();
esp_err_t leitor_tensao_ler(uint16_t* valor_bruto);
