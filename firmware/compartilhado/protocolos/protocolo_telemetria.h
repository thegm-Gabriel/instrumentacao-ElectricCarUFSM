#pragma once

#include <stddef.h>
#include <stdint.h>

#define TELEMETRIA_INICIO_1 0xAAu
#define TELEMETRIA_INICIO_2 0x55u
#define TELEMETRIA_INICIO_CONFIRMACAO_2 0x56u
#define TELEMETRIA_VERSAO_PROTOCOLO 3u
#define TELEMETRIA_TIPO_DADOS 1u
#define TELEMETRIA_TIPO_CONFIRMACAO 2u
#define TELEMETRIA_TAMANHO_CARGA_DADOS 81u
#define TAMANHO_PACOTE_TELEMETRIA 88u
#define TAMANHO_CONFIRMACAO_TELEMETRIA 20u

#define CAPACIDADE_SATELITE_PAINEL_WEB (1u << 0)
#define CAPACIDADE_SATELITE_PORTAL_CATIVO (1u << 1)
#define CAPACIDADE_SATELITE_METRICAS_LINK (1u << 2)
#define CAPACIDADE_SATELITE_COMANDOS (1u << 3)
#define CAPACIDADE_SATELITE_OTA_UART (1u << 4)

typedef enum {
    SATELITE_TELEMETRIA_DESCONHECIDO = 0,
    SATELITE_TELEMETRIA_EQUIPE = 1,
    SATELITE_TELEMETRIA_VISITANTES = 2,
} tipo_satelite_telemetria_t;

/*
 * Formato binario comum aos tres firmwares. Campos inteiros usam little-endian,
 * que e a ordem nativa dos ESP32. Alteracoes incompativeis exigem nova versao.
 */
typedef struct __attribute__((packed)) {
    uint8_t inicio_1;
    uint8_t inicio_2;
    uint8_t versao;
    uint8_t tipo;
    uint8_t tamanho_carga;
    uint16_t sequencia;
    uint32_t tempo_mestre_ms;
    uint16_t tensao_adc_bruta;
    int16_t aceleracao_x;
    int16_t aceleracao_y;
    int16_t aceleracao_z;
    int16_t giroscopio_x;
    int16_t giroscopio_y;
    int16_t giroscopio_z;
    uint8_t marcha; /* bits 0..6: marcha; bit 7: origem simulada */
    uint16_t velocidade_centesimos_kmh;
    int16_t aceleracao_milesimos_ms2;
    uint16_t acelerador_decimos_percentual;
    uint16_t freio_decimos_percentual;
    uint16_t tensao_pacote_mv;
    int16_t corrente_centesimos_a;
    uint16_t carga_decimos_percentual;
    uint16_t tensoes_celulas_mv[4];
    int32_t latitude_micrograus;
    int32_t longitude_micrograus;
    uint16_t rumo_decimos_grau;
    uint16_t distancias_cm[8];
    int16_t potencia_w;
    uint16_t autonomia_decimos_km;
    uint32_t percurso_metros;
    uint32_t odometro_metros;
    uint16_t crc16;
} pacote_telemetria_t;

/* Resposta curta do satelite para medir o tempo de ida e volta. */
typedef struct __attribute__((packed)) {
    uint8_t inicio_1;
    uint8_t inicio_2;
    uint8_t versao;
    uint8_t tipo;
    uint16_t sequencia;
    uint32_t tempo_mestre_ms;
    uint8_t tipo_satelite;
    uint8_t versao_maior;
    uint8_t versao_menor;
    uint8_t versao_correcao;
    uint32_t capacidades;
    uint16_t crc16;
} confirmacao_telemetria_t;

typedef enum {
    SEQUENCIA_TELEMETRIA_INICIAL,
    SEQUENCIA_TELEMETRIA_CONTINUA,
    SEQUENCIA_TELEMETRIA_COM_PERDA,
    SEQUENCIA_TELEMETRIA_DUPLICADA,
    SEQUENCIA_TELEMETRIA_FORA_DE_ORDEM,
} resultado_sequencia_telemetria_t;

#ifdef __cplusplus
static_assert(sizeof(pacote_telemetria_t) == TAMANHO_PACOTE_TELEMETRIA,
              "O pacote de telemetria v3 deve ter 88 bytes");
static_assert(sizeof(confirmacao_telemetria_t) == TAMANHO_CONFIRMACAO_TELEMETRIA,
              "A confirmacao de telemetria deve ter 20 bytes");
#else
_Static_assert(sizeof(pacote_telemetria_t) == TAMANHO_PACOTE_TELEMETRIA,
               "O pacote de telemetria v3 deve ter 88 bytes");
_Static_assert(sizeof(confirmacao_telemetria_t) == TAMANHO_CONFIRMACAO_TELEMETRIA,
               "A confirmacao de telemetria deve ter 20 bytes");
#endif

/* CRC-16/CCITT-FALSE: polinomio 0x1021, valor inicial 0xFFFF. */
static inline uint16_t protocolo_telemetria_calcular_crc16(
    const pacote_telemetria_t *pacote)
{
    if (pacote == NULL) return 0;

    const uint8_t *bytes = (const uint8_t *)pacote;
    uint16_t crc = 0xFFFFu;
    for (size_t indice = 0; indice < sizeof(*pacote) - sizeof(pacote->crc16); ++indice) {
        crc ^= (uint16_t)bytes[indice] << 8;
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000u) != 0u ? (uint16_t)((crc << 1) ^ 0x1021u)
                                        : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static inline uint16_t protocolo_telemetria_calcular_crc_confirmacao(
    const confirmacao_telemetria_t *confirmacao)
{
    if (confirmacao == NULL) return 0;
    const uint8_t *bytes = (const uint8_t *)confirmacao;
    uint16_t crc = 0xFFFFu;
    for (size_t indice = 0;
         indice < sizeof(*confirmacao) - sizeof(confirmacao->crc16); ++indice) {
        crc ^= (uint16_t)bytes[indice] << 8;
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000u) != 0u ? (uint16_t)((crc << 1) ^ 0x1021u)
                                        : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static inline int protocolo_telemetria_confirmacao_valida(
    const confirmacao_telemetria_t *confirmacao)
{
    return confirmacao != NULL && confirmacao->inicio_1 == TELEMETRIA_INICIO_1 &&
           confirmacao->inicio_2 == TELEMETRIA_INICIO_CONFIRMACAO_2 &&
           confirmacao->versao == TELEMETRIA_VERSAO_PROTOCOLO &&
           confirmacao->tipo == TELEMETRIA_TIPO_CONFIRMACAO &&
           confirmacao->tipo_satelite >= SATELITE_TELEMETRIA_EQUIPE &&
           confirmacao->tipo_satelite <= SATELITE_TELEMETRIA_VISITANTES &&
           protocolo_telemetria_calcular_crc_confirmacao(confirmacao) ==
               confirmacao->crc16;
}

static inline resultado_sequencia_telemetria_t
protocolo_telemetria_classificar_sequencia(int possui_anterior, uint16_t anterior,
                                           uint16_t atual, uint16_t *perdidos)
{
    if (perdidos != NULL) *perdidos = 0;
    if (!possui_anterior) return SEQUENCIA_TELEMETRIA_INICIAL;
    const uint16_t diferenca = (uint16_t)(atual - anterior);
    if (diferenca == 0u) return SEQUENCIA_TELEMETRIA_DUPLICADA;
    if (diferenca >= 0x8000u) return SEQUENCIA_TELEMETRIA_FORA_DE_ORDEM;
    if (diferenca == 1u) return SEQUENCIA_TELEMETRIA_CONTINUA;
    if (perdidos != NULL) *perdidos = (uint16_t)(diferenca - 1u);
    return SEQUENCIA_TELEMETRIA_COM_PERDA;
}

static inline int protocolo_telemetria_cabecalho_valido(
    const pacote_telemetria_t *pacote)
{
    return pacote != NULL && pacote->inicio_1 == TELEMETRIA_INICIO_1 &&
           pacote->inicio_2 == TELEMETRIA_INICIO_2 &&
           pacote->versao == TELEMETRIA_VERSAO_PROTOCOLO &&
           pacote->tipo == TELEMETRIA_TIPO_DADOS &&
           pacote->tamanho_carga == TELEMETRIA_TAMANHO_CARGA_DADOS;
}

static inline int protocolo_telemetria_pacote_valido(
    const pacote_telemetria_t *pacote)
{
    return protocolo_telemetria_cabecalho_valido(pacote) &&
           protocolo_telemetria_calcular_crc16(pacote) == pacote->crc16;
}

static inline int protocolo_telemetria_conteudo_valido(
    const pacote_telemetria_t *pacote)
{
    if (pacote == NULL) return 0;
    const uint8_t marcha = pacote->marcha & 0x7Fu;
    return pacote->tensao_adc_bruta <= 4095u && marcha <= 3u &&
           pacote->velocidade_centesimos_kmh <= 25000u &&
           pacote->acelerador_decimos_percentual <= 1000u &&
           pacote->freio_decimos_percentual <= 1000u &&
           pacote->carga_decimos_percentual <= 1000u &&
           pacote->rumo_decimos_grau <= 3600u &&
           pacote->latitude_micrograus >= -90000000 &&
           pacote->latitude_micrograus <= 90000000 &&
           pacote->longitude_micrograus >= -180000000 &&
           pacote->longitude_micrograus <= 180000000;
}
