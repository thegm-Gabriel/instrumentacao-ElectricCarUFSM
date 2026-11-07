// Contrato de preparação: não representa atuação sobre motor ou freio.
#pragma once
#include <stdint.h>

#define CONTROLE_REMOTO_FORMATO 1u
#define CONTROLE_REMOTO_VELOCIDADE_MAXIMA 3500u /* centésimos de km/h; faixa de preparação */
#define CONTROLE_REMOTO_ACELERACAO_MAXIMA 5000u /* milésimos de m/s²; faixa de preparação */

typedef enum {
    MARCHA_REMOTA_NEUTRO = 0,
    MARCHA_REMOTA_FRENTE = 1,
    MARCHA_REMOTA_RE = 2,
} marcha_remota_t;

typedef struct __attribute__((packed)) {
    uint8_t formato;
    uint8_t atuacao_habilitada; /* Sempre zero nesta implementação. */
    uint8_t marcha_solicitada;
    uint8_t parada_solicitada;
    uint32_t velocidade_maxima_centesimos_kmh;
    uint32_t aceleracao_maxima_milesimos_ms2;
    uint32_t revisao;
    uint32_t alterado_em_ms;
} estado_controle_remoto_t;
