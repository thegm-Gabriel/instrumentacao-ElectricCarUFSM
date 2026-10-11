// Reconhece e recompõe quadros de telemetria, comandos e OTA na UART.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "protocolo_comandos.h"
#include "protocolo_ota_satelites.h"

typedef enum {
    ENLACE_UART_AGUARDANDO = 0,
    ENLACE_UART_TELEMETRIA_COMPLETA = 1,
    ENLACE_UART_OTA_COMPLETA = 2,
    ENLACE_UART_COMANDO_COMPLETO = 3,
    ENLACE_UART_QUADRO_INVALIDO = 4,
} resultado_parser_enlace_uart_t;

typedef enum {
    ENLACE_UART_SEM_QUADRO = 0,
    ENLACE_UART_QUADRO_TELEMETRIA = 1,
    ENLACE_UART_QUADRO_OTA = 2,
    ENLACE_UART_QUADRO_COMANDO = 3,
} tipo_quadro_enlace_uart_t;

typedef struct {
    uint8_t quadro[OTA_SATELITE_TAMANHO_MAXIMO_QUADRO];
    size_t quantidade;
    size_t tamanho_esperado;
    size_t ultimo_tamanho;
    size_t tamanho_telemetria;
    uint8_t segundo_byte_telemetria;
    uint8_t tipo_em_recepcao;
} parser_enlace_uart_t;

#ifdef __cplusplus
static_assert(COMANDO_TAMANHO_MAXIMO_QUADRO <= OTA_SATELITE_TAMANHO_MAXIMO_QUADRO,
              "O buffer do enlace precisa comportar comandos");
#else
_Static_assert(COMANDO_TAMANHO_MAXIMO_QUADRO <= OTA_SATELITE_TAMANHO_MAXIMO_QUADRO,
               "O buffer do enlace precisa comportar comandos");
#endif

static inline void protocolo_enlace_uart_inicializar(
    parser_enlace_uart_t *parser, uint8_t segundo_byte_telemetria,
    size_t tamanho_telemetria)
{
    if (parser == NULL) return;
    memset(parser, 0, sizeof(*parser));
    parser->segundo_byte_telemetria = segundo_byte_telemetria;
    parser->tamanho_telemetria = tamanho_telemetria;
}

static inline void protocolo_enlace_uart_reiniciar(parser_enlace_uart_t *parser)
{
    if (parser == NULL) return;
    parser->quantidade = 0;
    parser->tamanho_esperado = 0;
    parser->tipo_em_recepcao = ENLACE_UART_SEM_QUADRO;
}

static inline int protocolo_enlace_uart_byte_inicial(uint8_t byte)
{
    return byte == 0xAAu || byte == OTA_SATELITE_INICIO_1 ||
           byte == COMANDO_INICIO_1;
}

static inline int protocolo_enlace_uart_inicio_valido(
    const parser_enlace_uart_t *parser, uint8_t primeiro, uint8_t segundo)
{
    return (primeiro == 0xAAu && segundo == parser->segundo_byte_telemetria) ||
           (primeiro == OTA_SATELITE_INICIO_1 &&
            segundo == OTA_SATELITE_INICIO_2) ||
           (primeiro == COMANDO_INICIO_1 && segundo == COMANDO_INICIO_2);
}

/*
 * Preserva um possível começo de quadro que tenha sido consumido como parte
 * de um quadro corrompido. Isso reduz a perda em cascata após remoção ou
 * inserção de bytes no enlace.
 */
static inline void protocolo_enlace_uart_reaproveitar_ultimo_quadro(
    parser_enlace_uart_t *parser)
{
    if (parser == NULL || parser->ultimo_tamanho == 0 ||
        parser->ultimo_tamanho > sizeof(parser->quadro)) {
        protocolo_enlace_uart_reiniciar(parser);
        return;
    }

    const size_t total = parser->ultimo_tamanho;
    size_t inicio = total;
    for (size_t indice = 1; indice + 1 < total; ++indice) {
        if (protocolo_enlace_uart_inicio_valido(
                parser, parser->quadro[indice], parser->quadro[indice + 1])) {
            inicio = indice;
            break;
        }
    }
    if (inicio == total && total > 1 &&
        protocolo_enlace_uart_byte_inicial(parser->quadro[total - 1])) {
        inicio = total - 1;
    }
    if (inicio == total) {
        protocolo_enlace_uart_reiniciar(parser);
        return;
    }

    const size_t restante = total - inicio;
    memmove(parser->quadro, parser->quadro + inicio, restante);
    parser->quantidade = restante;
    parser->tamanho_esperado = 0;
    parser->tipo_em_recepcao = ENLACE_UART_SEM_QUADRO;
    if (restante < 2) return;

    if (parser->quadro[0] == 0xAAu &&
        parser->quadro[1] == parser->segundo_byte_telemetria) {
        parser->tipo_em_recepcao = ENLACE_UART_QUADRO_TELEMETRIA;
        parser->tamanho_esperado = parser->tamanho_telemetria;
    } else if (parser->quadro[0] == OTA_SATELITE_INICIO_1 &&
               parser->quadro[1] == OTA_SATELITE_INICIO_2) {
        parser->tipo_em_recepcao = ENLACE_UART_QUADRO_OTA;
        if (restante >= OTA_SATELITE_TAMANHO_CABECALHO) {
            cabecalho_quadro_ota_satelite_t cabecalho;
            memcpy(&cabecalho, parser->quadro, sizeof(cabecalho));
            if (cabecalho.versao_protocolo == OTA_SATELITE_VERSAO_PROTOCOLO &&
                protocolo_ota_satelites_tipo_valido(cabecalho.tipo) &&
                cabecalho.tamanho_carga <= OTA_SATELITE_TAMANHO_MAXIMO_CARGA) {
                parser->tamanho_esperado =
                    protocolo_ota_satelites_tamanho_quadro(cabecalho.tamanho_carga);
            } else {
                protocolo_enlace_uart_reiniciar(parser);
            }
        }
    } else if (parser->quadro[0] == COMANDO_INICIO_1 &&
               parser->quadro[1] == COMANDO_INICIO_2) {
        parser->tipo_em_recepcao = ENLACE_UART_QUADRO_COMANDO;
        if (restante >= COMANDO_TAMANHO_CABECALHO) {
            cabecalho_quadro_comando_t cabecalho;
            memcpy(&cabecalho, parser->quadro, sizeof(cabecalho));
            if (cabecalho.versao_protocolo == COMANDO_VERSAO_PROTOCOLO &&
                (cabecalho.tipo == TIPO_QUADRO_COMANDO_SOLICITACAO ||
                 cabecalho.tipo == TIPO_QUADRO_COMANDO_RESPOSTA) &&
                protocolo_comandos_no_valido(cabecalho.origem) &&
                protocolo_comandos_no_valido(cabecalho.destino) &&
                cabecalho.tamanho_carga <= COMANDO_TAMANHO_MAXIMO_CARGA) {
                parser->tamanho_esperado =
                    protocolo_comandos_tamanho_quadro(cabecalho.tamanho_carga);
            } else {
                protocolo_enlace_uart_reiniciar(parser);
            }
        }
    }
}

static inline resultado_parser_enlace_uart_t protocolo_enlace_uart_processar_byte(
    parser_enlace_uart_t *parser, uint8_t byte)
{
    if (parser == NULL || parser->tamanho_telemetria < 2 ||
        parser->tamanho_telemetria > sizeof(parser->quadro)) {
        return ENLACE_UART_QUADRO_INVALIDO;
    }

    if (parser->quantidade == 0) {
        if (protocolo_enlace_uart_byte_inicial(byte)) {
            parser->quadro[0] = byte;
            parser->quantidade = 1;
        }
        return ENLACE_UART_AGUARDANDO;
    }

    if (parser->quantidade == 1) {
        const uint8_t primeiro = parser->quadro[0];
        if (primeiro == 0xAAu && byte == parser->segundo_byte_telemetria) {
            parser->quadro[1] = byte;
            parser->quantidade = 2;
            parser->tamanho_esperado = parser->tamanho_telemetria;
            parser->tipo_em_recepcao = ENLACE_UART_QUADRO_TELEMETRIA;
        } else if (primeiro == OTA_SATELITE_INICIO_1 &&
                   byte == OTA_SATELITE_INICIO_2) {
            parser->quadro[1] = byte;
            parser->quantidade = 2;
            parser->tipo_em_recepcao = ENLACE_UART_QUADRO_OTA;
        } else if (primeiro == COMANDO_INICIO_1 && byte == COMANDO_INICIO_2) {
            parser->quadro[1] = byte;
            parser->quantidade = 2;
            parser->tipo_em_recepcao = ENLACE_UART_QUADRO_COMANDO;
        } else if (protocolo_enlace_uart_byte_inicial(byte)) {
            parser->quadro[0] = byte;
        } else {
            protocolo_enlace_uart_reiniciar(parser);
        }
        return ENLACE_UART_AGUARDANDO;
    }

    if (parser->quantidade >= sizeof(parser->quadro)) {
        parser->ultimo_tamanho = parser->quantidade;
        protocolo_enlace_uart_reaproveitar_ultimo_quadro(parser);
        return ENLACE_UART_QUADRO_INVALIDO;
    }
    parser->quadro[parser->quantidade++] = byte;

    if (parser->tipo_em_recepcao == ENLACE_UART_QUADRO_OTA &&
        parser->quantidade == OTA_SATELITE_TAMANHO_CABECALHO) {
        cabecalho_quadro_ota_satelite_t cabecalho;
        memcpy(&cabecalho, parser->quadro, sizeof(cabecalho));
        if (cabecalho.versao_protocolo != OTA_SATELITE_VERSAO_PROTOCOLO ||
            !protocolo_ota_satelites_tipo_valido(cabecalho.tipo) ||
            cabecalho.tamanho_carga > OTA_SATELITE_TAMANHO_MAXIMO_CARGA) {
            parser->ultimo_tamanho = parser->quantidade;
            protocolo_enlace_uart_reaproveitar_ultimo_quadro(parser);
            return ENLACE_UART_QUADRO_INVALIDO;
        }
        parser->tamanho_esperado =
            protocolo_ota_satelites_tamanho_quadro(cabecalho.tamanho_carga);
    }
    if (parser->tipo_em_recepcao == ENLACE_UART_QUADRO_COMANDO &&
        parser->quantidade == COMANDO_TAMANHO_CABECALHO) {
        cabecalho_quadro_comando_t cabecalho;
        memcpy(&cabecalho, parser->quadro, sizeof(cabecalho));
        if (cabecalho.versao_protocolo != COMANDO_VERSAO_PROTOCOLO ||
            (cabecalho.tipo != TIPO_QUADRO_COMANDO_SOLICITACAO &&
             cabecalho.tipo != TIPO_QUADRO_COMANDO_RESPOSTA) ||
            !protocolo_comandos_no_valido(cabecalho.origem) ||
            !protocolo_comandos_no_valido(cabecalho.destino) ||
            cabecalho.tamanho_carga > COMANDO_TAMANHO_MAXIMO_CARGA) {
            parser->ultimo_tamanho = parser->quantidade;
            protocolo_enlace_uart_reaproveitar_ultimo_quadro(parser);
            return ENLACE_UART_QUADRO_INVALIDO;
        }
        parser->tamanho_esperado =
            protocolo_comandos_tamanho_quadro(cabecalho.tamanho_carga);
    }

    if (parser->tamanho_esperado == 0 ||
        parser->quantidade < parser->tamanho_esperado) {
        return ENLACE_UART_AGUARDANDO;
    }

    parser->ultimo_tamanho = parser->tamanho_esperado;
    const uint8_t tipo = parser->tipo_em_recepcao;
    const int ota_valida = tipo != ENLACE_UART_QUADRO_OTA ||
        protocolo_ota_satelites_validar_quadro(parser->quadro,
                                                parser->ultimo_tamanho);
    const int comando_valido = tipo != ENLACE_UART_QUADRO_COMANDO ||
        protocolo_comandos_validar_quadro(parser->quadro,
                                           parser->ultimo_tamanho);
    if (!ota_valida || !comando_valido) {
        protocolo_enlace_uart_reaproveitar_ultimo_quadro(parser);
        return ENLACE_UART_QUADRO_INVALIDO;
    }
    protocolo_enlace_uart_reiniciar(parser);
    if (tipo == ENLACE_UART_QUADRO_OTA) return ENLACE_UART_OTA_COMPLETA;
    if (tipo == ENLACE_UART_QUADRO_COMANDO) return ENLACE_UART_COMANDO_COMPLETO;
    return ENLACE_UART_TELEMETRIA_COMPLETA;
}
