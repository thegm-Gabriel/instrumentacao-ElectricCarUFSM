#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "protocolo_telemetria.h"

typedef struct {
    bool valido;
    uint16_t sequencia;
    uint32_t tempo_mestre_ms;
    uint16_t tensao_adc_bruta;
    int16_t aceleracao_x;
    int16_t aceleracao_y;
    int16_t aceleracao_z;
    int16_t giroscopio_x;
    int16_t giroscopio_y;
    int16_t giroscopio_z;
    uint32_t pacotes_validos;
    uint32_t pacotes_invalidos;
    uint32_t falhas_checksum;
    uint32_t falhas_conteudo;
    uint32_t pacotes_perdidos;
    uint32_t pacotes_duplicados;
    uint32_t reinicios_mestre;
    uint32_t ultimo_recebimento_ms;
} dados_telemetria_t;
