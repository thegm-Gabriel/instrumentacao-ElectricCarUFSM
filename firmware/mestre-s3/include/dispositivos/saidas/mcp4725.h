// Conversor DAC do acelerador; escreve apenas RAM, nunca a EEPROM.
#pragma once

#include <cstdint>
#include "esp_err.h"

esp_err_t mcp4725_iniciar();
esp_err_t mcp4725_definir_milivolts(uint16_t milivolts);
bool mcp4725_disponivel();
