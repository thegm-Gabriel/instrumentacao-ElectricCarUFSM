// Modela o conjunto de dados veiculares publicado pela telemetria.
#pragma once

#include <cstdint>

enum class MarchaVeiculo : uint8_t {
    Estacionado = 0,
    Neutro,
    Frente,
    Re,
};

struct DadosTelemetriaVeiculo {
    uint16_t tensao_adc_bruta = 0;
    int16_t aceleracao_x = 0;
    int16_t aceleracao_y = 0;
    int16_t aceleracao_z = 0;
    int16_t giroscopio_x = 0;
    int16_t giroscopio_y = 0;
    int16_t giroscopio_z = 0;

    // Valores físicos locais. O protocolo atual transmite a aceleração
    // longitudinal em m/s²; o valor em g é mantido para diagnóstico no mestre.
    float aceleracao_x_g = 0.0f;
    float aceleracao_y_g = 0.0f;
    float aceleracao_z_g = 0.0f;
    float aceleracao_x_ms2 = 0.0f;
    float aceleracao_y_ms2 = 0.0f;
    float aceleracao_z_ms2 = 0.0f;
    float giroscopio_x_graus_s = 0.0f;
    float giroscopio_y_graus_s = 0.0f;
    float giroscopio_z_graus_s = 0.0f;
    float temperatura_mpu_c = 0.0f;
    bool mpu_ativo = false;
    bool mpu_calibrado = false;
    bool eixo_longitudinal_compativel = false;
    uint8_t eixo_longitudinal_mpu = 0;
    // -1: aceleração para trás; 0: zona neutra; +1: para frente.
    int8_t sentido_aceleracao_longitudinal = 0;

    MarchaVeiculo marcha = MarchaVeiculo::Estacionado;
    float velocidade_kmh = 0.0f;
    float aceleracao_ms2 = 0.0f;
    float aceleracao_g = 0.0f;
    float acelerador_percentual = 0.0f;
    float freio_percentual = 0.0f;
    float tensao_pacote_v = 0.0f;
    float corrente_a = 0.0f;
    float carga_percentual = 0.0f;
    float tensoes_celulas_v[4] = {};
    double latitude = 0.0;
    double longitude = 0.0;
    float rumo_graus = 0.0f;
    float distancias_cm[8] = {};
    float potencia_w = 0.0f;
    float autonomia_km = 0.0f;
    float percurso_km = 0.0f;
    float odometro_km = 0.0f;
    bool simulado = false;
};
