#pragma once

#include <stddef.h>
#include <stdint.h>

#define TELEMETRIA_INICIO_1 0xAAu
#define TELEMETRIA_INICIO_2 0x55u
#define TAMANHO_PACOTE_TELEMETRIA 23u

/* Formato binario comum aos tres firmwares. Alteracoes exigem nova versao do protocolo. */
typedef struct __attribute__((packed)) {
    uint8_t inicio_1;
    uint8_t inicio_2;
    uint16_t sequencia;
    uint32_t tempo_mestre_ms;
    uint16_t tensao_adc_bruta;
    int16_t aceleracao_x;
    int16_t aceleracao_y;
    int16_t aceleracao_z;
    int16_t giroscopio_x;
    int16_t giroscopio_y;
    int16_t giroscopio_z;
    uint8_t checksum;
} pacote_telemetria_t;

#ifdef __cplusplus
static_assert(sizeof(pacote_telemetria_t) == TAMANHO_PACOTE_TELEMETRIA,
              "O pacote de telemetria deve ter 23 bytes");
#else
_Static_assert(sizeof(pacote_telemetria_t) == TAMANHO_PACOTE_TELEMETRIA,
               "O pacote de telemetria deve ter 23 bytes");
#endif

static inline uint8_t protocolo_telemetria_calcular_checksum(
    const pacote_telemetria_t *pacote)
{
    if (pacote == NULL) return 0;

    const uint8_t *bytes = (const uint8_t *)pacote;
    uint8_t checksum = 0;
    for (size_t indice = 0; indice < sizeof(*pacote) - 1; indice++) {
        checksum ^= bytes[indice];
    }
    return checksum;
}

static inline int protocolo_telemetria_cabecalho_valido(
    const pacote_telemetria_t *pacote)
{
    return pacote != NULL && pacote->inicio_1 == TELEMETRIA_INICIO_1 &&
           pacote->inicio_2 == TELEMETRIA_INICIO_2;
}
