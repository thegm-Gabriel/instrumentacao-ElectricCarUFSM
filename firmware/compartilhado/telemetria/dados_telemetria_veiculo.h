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

    MarchaVeiculo marcha = MarchaVeiculo::Estacionado;
    float velocidade_kmh = 0.0f;
    float aceleracao_ms2 = 0.0f;
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
