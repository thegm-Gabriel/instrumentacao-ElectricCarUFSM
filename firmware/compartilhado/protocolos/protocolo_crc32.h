#pragma once

#include <stddef.h>
#include <stdint.h>

/* CRC-32/ISO-HDLC, usado pelos protocolos variáveis do enlace UART. */
static inline uint32_t protocolo_calcular_crc32(const void *dados, size_t tamanho)
{
    if (dados == NULL && tamanho != 0) return 0;
    const uint8_t *bytes = (const uint8_t *)dados;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t indice = 0; indice < tamanho; ++indice) {
        crc ^= bytes[indice];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) != 0u ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
        }
    }
    return crc ^ 0xFFFFFFFFu;
}
