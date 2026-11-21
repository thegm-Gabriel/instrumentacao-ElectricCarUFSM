// Leitura calibrada do monitor elétrico INA228 no barramento I2C do mestre.
#pragma once

#include <cstdint>

#include "esp_err.h"

struct LeituraIna228 {
    bool valida = false;
    bool energia_valida = false;
    bool carga_valida = false;
    float tensao_barramento_v = 0.0f;
    float tensao_shunt_mv = 0.0f;
    float corrente_a = 0.0f;  // Positiva: consumo; negativa: regeneração.
    float potencia_w = 0.0f;  // Assinada, calculada de VBUS × corrente.
    float potencia_chip_w = 0.0f;  // Registrador POWER: magnitude sem sinal.
    float temperatura_chip_c = 0.0f;
    double energia_j = 0.0;  // Acumulador absoluto do INA228 desde o reset.
    double carga_c = 0.0;  // Acumulador assinado desde o reset.
    uint16_t diagnostico = 0;
    uint64_t instante_ms = 0;
};

struct EstadoIna228 {
    bool inicializado = false;
    uint16_t identificacao = 0;
    uint16_t calibracao = 0;
    uint32_t leituras_ok = 0;
    uint32_t erros_leitura = 0;
    uint32_t erros_consecutivos = 0;
    esp_err_t ultimo_erro = ESP_OK;
    LeituraIna228 ultima_leitura{};
};

esp_err_t ina228_iniciar();
esp_err_t ina228_ler(LeituraIna228* destino);
EstadoIna228 ina228_obter_estado();
