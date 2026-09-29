#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "protocolo_crc32.h"

#define OTA_SATELITE_INICIO_1 0xA5u
#define OTA_SATELITE_INICIO_2 0x5Au
#define OTA_SATELITE_VERSAO_PROTOCOLO 1u
#define OTA_SATELITE_TAMANHO_MAXIMO_CARGA 2048u
#define OTA_SATELITE_TAMANHO_VERSAO 24u
#define OTA_SATELITE_TAMANHO_SHA256 32u

typedef enum {
    OTA_SATELITE_TIPO_INICIAR = 1,
    OTA_SATELITE_TIPO_DADOS = 2,
    OTA_SATELITE_TIPO_FINALIZAR = 3,
    OTA_SATELITE_TIPO_CANCELAR = 4,
    OTA_SATELITE_TIPO_RESPOSTA = 5,
} tipo_quadro_ota_satelite_t;

typedef enum {
    OTA_SATELITE_FASE_OCIOSO = 0,
    OTA_SATELITE_FASE_RECEBENDO = 1,
    OTA_SATELITE_FASE_VALIDANDO = 2,
    OTA_SATELITE_FASE_PRONTO_PARA_REINICIAR = 3,
    OTA_SATELITE_FASE_CANCELADO = 4,
    OTA_SATELITE_FASE_FALHA = 5,
} fase_ota_satelite_t;

typedef enum {
    OTA_SATELITE_RESPOSTA_OK = 0,
    OTA_SATELITE_RESPOSTA_SESSAO_INVALIDA = 1,
    OTA_SATELITE_RESPOSTA_SEQUENCIA_INVALIDA = 2,
    OTA_SATELITE_RESPOSTA_DESLOCAMENTO_INVALIDO = 3,
    OTA_SATELITE_RESPOSTA_TAMANHO_INVALIDO = 4,
    OTA_SATELITE_RESPOSTA_GRAVACAO_FALHOU = 5,
    OTA_SATELITE_RESPOSTA_INTEGRIDADE_FALHOU = 6,
    OTA_SATELITE_RESPOSTA_ESTADO_INVALIDO = 7,
    OTA_SATELITE_RESPOSTA_PROTOCOLO_INVALIDO = 8,
} codigo_resposta_ota_satelite_t;

/* Cabeçalho comum. Os inteiros usam little-endian, ordem nativa dos ESP32. */
typedef struct __attribute__((packed)) {
    uint8_t inicio_1;
    uint8_t inicio_2;
    uint8_t versao_protocolo;
    uint8_t tipo;
    uint32_t sessao;
    uint32_t sequencia;
    uint32_t deslocamento;
    uint16_t tamanho_carga;
} cabecalho_quadro_ota_satelite_t;

typedef struct __attribute__((packed)) {
    uint32_t tamanho_firmware;
    uint8_t sha256[OTA_SATELITE_TAMANHO_SHA256];
    char versao[OTA_SATELITE_TAMANHO_VERSAO];
} carga_inicio_ota_satelite_t;

typedef struct __attribute__((packed)) {
    uint8_t codigo;
    uint8_t fase;
    int32_t erro_esp;
} carga_resposta_ota_satelite_t;

#define OTA_SATELITE_TAMANHO_CABECALHO ((size_t)sizeof(cabecalho_quadro_ota_satelite_t))
#define OTA_SATELITE_TAMANHO_CRC ((size_t)sizeof(uint32_t))
#define OTA_SATELITE_TAMANHO_MAXIMO_QUADRO \
    (OTA_SATELITE_TAMANHO_CABECALHO + OTA_SATELITE_TAMANHO_MAXIMO_CARGA + \
     OTA_SATELITE_TAMANHO_CRC)

typedef enum {
    OTA_SATELITE_PARSER_AGUARDANDO = 0,
    OTA_SATELITE_PARSER_QUADRO_COMPLETO = 1,
    OTA_SATELITE_PARSER_QUADRO_INVALIDO = 2,
} resultado_parser_ota_satelite_t;

typedef struct {
    uint8_t quadro[OTA_SATELITE_TAMANHO_MAXIMO_QUADRO];
    size_t quantidade;
    size_t tamanho_esperado;
} parser_ota_satelite_t;

#ifdef __cplusplus
static_assert(sizeof(cabecalho_quadro_ota_satelite_t) == 18,
              "O cabeçalho OTA deve ter 18 bytes");
static_assert(sizeof(carga_inicio_ota_satelite_t) == 60,
              "A carga inicial OTA deve ter 60 bytes");
static_assert(sizeof(carga_resposta_ota_satelite_t) == 6,
              "A resposta OTA deve ter 6 bytes");
#else
_Static_assert(sizeof(cabecalho_quadro_ota_satelite_t) == 18,
               "O cabeçalho OTA deve ter 18 bytes");
_Static_assert(sizeof(carga_inicio_ota_satelite_t) == 60,
               "A carga inicial OTA deve ter 60 bytes");
_Static_assert(sizeof(carga_resposta_ota_satelite_t) == 6,
               "A resposta OTA deve ter 6 bytes");
#endif

static inline uint32_t protocolo_ota_satelites_calcular_crc32(
    const void *dados, size_t tamanho)
{
    return protocolo_calcular_crc32(dados, tamanho);
}

static inline size_t protocolo_ota_satelites_tamanho_quadro(uint16_t tamanho_carga)
{
    return OTA_SATELITE_TAMANHO_CABECALHO + tamanho_carga + OTA_SATELITE_TAMANHO_CRC;
}

static inline int protocolo_ota_satelites_tipo_valido(uint8_t tipo)
{
    return tipo >= OTA_SATELITE_TIPO_INICIAR && tipo <= OTA_SATELITE_TIPO_RESPOSTA;
}

static inline int protocolo_ota_satelites_validar_quadro(
    const uint8_t *quadro, size_t tamanho)
{
    if (quadro == NULL || tamanho < OTA_SATELITE_TAMANHO_CABECALHO + OTA_SATELITE_TAMANHO_CRC)
        return 0;
    cabecalho_quadro_ota_satelite_t cabecalho;
    memcpy(&cabecalho, quadro, sizeof(cabecalho));
    if (cabecalho.inicio_1 != OTA_SATELITE_INICIO_1 ||
        cabecalho.inicio_2 != OTA_SATELITE_INICIO_2 ||
        cabecalho.versao_protocolo != OTA_SATELITE_VERSAO_PROTOCOLO ||
        !protocolo_ota_satelites_tipo_valido(cabecalho.tipo) ||
        cabecalho.tamanho_carga > OTA_SATELITE_TAMANHO_MAXIMO_CARGA ||
        tamanho != protocolo_ota_satelites_tamanho_quadro(cabecalho.tamanho_carga)) {
        return 0;
    }
    uint32_t crc_recebido = 0;
    memcpy(&crc_recebido, quadro + tamanho - OTA_SATELITE_TAMANHO_CRC,
           sizeof(crc_recebido));
    return crc_recebido ==
           protocolo_ota_satelites_calcular_crc32(quadro, tamanho - OTA_SATELITE_TAMANHO_CRC);
}

static inline size_t protocolo_ota_satelites_montar_quadro(
    uint8_t *destino, size_t capacidade, uint8_t tipo, uint32_t sessao,
    uint32_t sequencia, uint32_t deslocamento, const void *carga,
    uint16_t tamanho_carga)
{
    if (destino == NULL || !protocolo_ota_satelites_tipo_valido(tipo) ||
        tamanho_carga > OTA_SATELITE_TAMANHO_MAXIMO_CARGA ||
        (tamanho_carga != 0 && carga == NULL)) {
        return 0;
    }
    const size_t tamanho = protocolo_ota_satelites_tamanho_quadro(tamanho_carga);
    if (capacidade < tamanho) return 0;
    const cabecalho_quadro_ota_satelite_t cabecalho = {
        OTA_SATELITE_INICIO_1, OTA_SATELITE_INICIO_2,
        OTA_SATELITE_VERSAO_PROTOCOLO, tipo, sessao, sequencia,
        deslocamento, tamanho_carga,
    };
    memcpy(destino, &cabecalho, sizeof(cabecalho));
    if (tamanho_carga != 0) memcpy(destino + sizeof(cabecalho), carga, tamanho_carga);
    const uint32_t crc = protocolo_ota_satelites_calcular_crc32(
        destino, tamanho - OTA_SATELITE_TAMANHO_CRC);
    memcpy(destino + tamanho - OTA_SATELITE_TAMANHO_CRC, &crc, sizeof(crc));
    return tamanho;
}

static inline resultado_parser_ota_satelite_t protocolo_ota_satelites_processar_byte(
    parser_ota_satelite_t *parser, uint8_t byte)
{
    if (parser == NULL) return OTA_SATELITE_PARSER_QUADRO_INVALIDO;
    if (parser->quantidade == 0) {
        if (byte == OTA_SATELITE_INICIO_1) parser->quadro[parser->quantidade++] = byte;
        return OTA_SATELITE_PARSER_AGUARDANDO;
    }
    if (parser->quantidade == 1) {
        if (byte == OTA_SATELITE_INICIO_2) {
            parser->quadro[parser->quantidade++] = byte;
        } else if (byte == OTA_SATELITE_INICIO_1) {
            parser->quadro[0] = byte;
        } else {
            parser->quantidade = 0;
        }
        return OTA_SATELITE_PARSER_AGUARDANDO;
    }

    if (parser->quantidade >= sizeof(parser->quadro)) {
        parser->quantidade = 0;
        parser->tamanho_esperado = 0;
        return OTA_SATELITE_PARSER_QUADRO_INVALIDO;
    }
    parser->quadro[parser->quantidade++] = byte;
    if (parser->quantidade == OTA_SATELITE_TAMANHO_CABECALHO) {
        cabecalho_quadro_ota_satelite_t cabecalho;
        memcpy(&cabecalho, parser->quadro, sizeof(cabecalho));
        if (cabecalho.versao_protocolo != OTA_SATELITE_VERSAO_PROTOCOLO ||
            !protocolo_ota_satelites_tipo_valido(cabecalho.tipo) ||
            cabecalho.tamanho_carga > OTA_SATELITE_TAMANHO_MAXIMO_CARGA) {
            parser->quantidade = 0;
            parser->tamanho_esperado = 0;
            return OTA_SATELITE_PARSER_QUADRO_INVALIDO;
        }
        parser->tamanho_esperado =
            protocolo_ota_satelites_tamanho_quadro(cabecalho.tamanho_carga);
    }
    if (parser->tamanho_esperado == 0 || parser->quantidade < parser->tamanho_esperado)
        return OTA_SATELITE_PARSER_AGUARDANDO;

    const int valido = protocolo_ota_satelites_validar_quadro(
        parser->quadro, parser->tamanho_esperado);
    parser->quantidade = 0;
    parser->tamanho_esperado = 0;
    return valido ? OTA_SATELITE_PARSER_QUADRO_COMPLETO
                  : OTA_SATELITE_PARSER_QUADRO_INVALIDO;
}
