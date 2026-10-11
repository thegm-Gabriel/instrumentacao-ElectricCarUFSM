// Declara a configuração e leitura do sensor inercial MPU6050.
#pragma once

#include <cstdint>

#include "esp_err.h"

struct LeituraMpu6050 {
    // Valores nativos preservados para diagnóstico e compatibilidade com o
    // protocolo de telemetria existente.
    int16_t aceleracao_x = 0;
    int16_t aceleracao_y = 0;
    int16_t aceleracao_z = 0;
    int16_t giroscopio_x = 0;
    int16_t giroscopio_y = 0;
    int16_t giroscopio_z = 0;

    // Grandezas físicas calculadas conforme a faixa configurada no sensor.
    float aceleracao_x_g = 0.0f;
    float aceleracao_y_g = 0.0f;
    float aceleracao_z_g = 0.0f;
    float aceleracao_x_ms2 = 0.0f;
    float aceleracao_y_ms2 = 0.0f;
    float aceleracao_z_ms2 = 0.0f;
    float giroscopio_x_graus_s = 0.0f;
    float giroscopio_y_graus_s = 0.0f;
    float giroscopio_z_graus_s = 0.0f;
    float temperatura_c = 0.0f;
};

struct EstadoMpu6050 {
    bool inicializado = false;
    bool configuracao_verificada = false;
    uint8_t identificacao = 0;
    uint16_t frequencia_amostragem_hz = 0;
    uint32_t leituras_ok = 0;
    uint32_t erros_leitura = 0;
    esp_err_t ultimo_erro = ESP_OK;
};

esp_err_t mpu6050_iniciar(uint32_t frequencia_amostragem_hz);
esp_err_t mpu6050_ler(LeituraMpu6050* leitura);
EstadoMpu6050 mpu6050_obter_estado();
