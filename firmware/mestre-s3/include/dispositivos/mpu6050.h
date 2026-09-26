#pragma once

#include <cstdint>

#include "esp_err.h"

struct LeituraMpu6050 {
    int16_t aceleracao_x = 0;
    int16_t aceleracao_y = 0;
    int16_t aceleracao_z = 0;
    int16_t giroscopio_x = 0;
    int16_t giroscopio_y = 0;
    int16_t giroscopio_z = 0;
};

esp_err_t mpu6050_iniciar();
esp_err_t mpu6050_ler(LeituraMpu6050* leitura);
