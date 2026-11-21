#pragma once

#include "esp_err.h"
#include <cstdint>

esp_err_t chave_marcha_iniciar();
// Retorna L=1, M=2, H=3. Os dois fios em nível baixo são inválidos.
esp_err_t chave_marcha_ler(uint8_t* marcha);
