// Contrato versionado do controle lógico. Nenhuma saída física é presumida.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define CONTROLE_REMOTO_FORMATO_LEGADO 6u
#define CONTROLE_REMOTO_FORMATO_REARME 7u
#define CONTROLE_REMOTO_FORMATO 8u
#define CONTROLE_REMOTO_VELOCIDADE_MAXIMA 3500u /* centésimos de km/h */
#define CONTROLE_REMOTO_ACELERADOR_MAXIMO 1000u /* décimos de % */
#define CONTROLE_REMOTO_EXPIRACAO_PEDAL_MS 300u
#define CONTROLE_REMOTO_REARME_MINIMO_MS 5000u
#define CONTROLE_REMOTO_ARMAR_DAC_ISOLADO 3u
#define CONTROLE_REMOTO_PERIODO_MS 20u
#define CONTROLE_REMOTO_ZONA_MORTA_MAXIMA 300u /* décimos de % */
#define CONTROLE_REMOTO_TENSAO_MINIMA_MAXIMA_MV 2500u

typedef enum {
    FAIXA_MARCHA_INDEFINIDA = 0,
    FAIXA_MARCHA_L = 1,
    FAIXA_MARCHA_M = 2,
    FAIXA_MARCHA_H = 3,
} faixa_marcha_remota_t;

typedef enum {
    CONTROLE_ESTADO_PARADO = 0,
    CONTROLE_ESTADO_BLOQUEADO = 1,
    CONTROLE_ESTADO_PRONTO = 2,
    CONTROLE_ESTADO_ACELERANDO = 3,
    CONTROLE_ESTADO_FALHA = 4,
    CONTROLE_ESTADO_LIMITANDO_VELOCIDADE = 5,
} estado_operacao_remota_t;

#define CONTROLE_BLOQUEIO_SEM_FAIXA (1u << 0)
#define CONTROLE_BLOQUEIO_SEM_LIMITES (1u << 1)
#define CONTROLE_BLOQUEIO_SEM_SENSORES (1u << 2)
#define CONTROLE_BLOQUEIO_TELEMETRIA_SIMULADA (1u << 3)
#define CONTROLE_BLOQUEIO_VELOCIDADE (1u << 4)
#define CONTROLE_BLOQUEIO_PARTIDA_OU_OTA (1u << 5)
#define CONTROLE_BLOQUEIO_PEDAL_INDISPONIVEL (1u << 6)
#define CONTROLE_BLOQUEIO_FALHA_TRAVADA (1u << 7)

typedef struct __attribute__((packed)) {
    uint8_t formato;
    uint8_t atuacao_habilitada; /* Zero até implementar e validar os pinos. */
    uint8_t faixa_marcha_solicitada;
    uint8_t faixa_marcha_fisica;
    uint8_t faixa_marcha_maxima;
    uint8_t parada_solicitada;
    uint8_t corte_por_rele_solicitado;
    uint8_t modo_a_aterrar_solicitado;
    uint8_t modo_b_aterrar_solicitado;
    uint8_t reversao_solicitada;
    uint8_t reversao_acionar_solicitado;
    uint8_t estado_operacao;
    uint8_t bloqueios;
    uint16_t acelerador_maximo_decimos_percentual;
    uint16_t acelerador_lido_decimos_percentual;
    uint16_t acelerador_permitido_decimos_percentual;
    uint32_t velocidade_maxima_centesimos_kmh;
    uint32_t velocidade_medida_centesimos_kmh;
    int32_t aceleracao_medida_milesimos_ms2;
    uint32_t parada_restante_ms;
    uint32_t amostra_pedal_restante_ms;
    uint32_t revisao;
    uint32_t alterado_em_ms;
    uint16_t zona_morta_decimos_percentual;
    uint16_t tensao_minima_saida_mv;
    uint16_t tensao_pedal_mv;
    uint16_t tensao_dac_mv;
    uint8_t dac_disponivel;
    uint8_t pedal_disponivel;
    uint8_t partida_concluida;
    uint8_t armamento_inicial_pendente; /* Requer armar após partida, parada ou desarme de proteção. */
    uint8_t ota_em_andamento;
    uint8_t dac_zero_confirmado; /* Confirma escrita I2C, não mede VOUT. */
    uint8_t reversao_permitida;
    uint32_t rearme_restante_ms;
    uint8_t dac_isolado_habilitado;
} estado_controle_remoto_t;

#define CONTROLE_REMOTO_TAMANHO_LEGADO offsetof(estado_controle_remoto_t, reversao_permitida)
#define CONTROLE_REMOTO_TAMANHO_REARME offsetof(estado_controle_remoto_t, dac_isolado_habilitado)
