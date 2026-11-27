#pragma once
#include <stdint.h>

// Ordem no quadro UART. Valores fracionários são transportados em milésimos.
#define AJUSTES_DESENVOLVEDOR_BASE(CAMPO) \
    CAMPO(uint16_t, pedal_inicio_mv, 1, 100, 2500) \
    CAMPO(uint16_t, pedal_fim_mv, 1, 200, 3000) \
    CAMPO(uint16_t, pedal_solto_mv, 1, 101, 2800) \
    CAMPO(uint16_t, pedal_margem_mv, 1, 0, 400) \
    CAMPO(uint16_t, zona_morta_decimos, 1, 0, 300) \
    CAMPO(uint16_t, dac_minimo_mv, 1, 0, 2500) \
    CAMPO(uint16_t, dac_maximo_mv, 1, 100, 5000) \
    CAMPO(uint32_t, tempo_pedal_solto_ms, 1, 250, 3000) \
    CAMPO(uint16_t, rampa_decimos_por_s, 1, 10, 1000) \
    CAMPO(uint32_t, faixa_limitador_centesimos, 1, 1, 1000) \
    CAMPO(uint32_t, margem_limitador_centesimos, 1, 1, 500) \
    CAMPO(uint8_t, amostras_retorno_limitador, 1, 1, 20) \
    CAMPO(uint32_t, velocidade_armamento_centesimos, 1, 0, 100) \
    CAMPO(uint32_t, espera_rearme_ms, 1, 5000, 30000) \
    CAMPO(uint32_t, atualizacao_dac_ms, 1, 20, 1000) \
    CAMPO(uint32_t, ressondagem_dac_ms, 1, 100, 30000) \
    CAMPO(float, diametro_roda_esquerda_m, 1000, 50, 2000) \
    CAMPO(float, diametro_roda_direita_m, 1000, 50, 2000) \
    CAMPO(float, giros_encoder_esquerdo_por_giro_roda, 1000, 50, 100000) \
    CAMPO(float, giros_encoder_direito_por_giro_roda, 1000, 50, 100000) \
    CAMPO(float, fator_encoder_esquerdo, 1000, 500, 2000) \
    CAMPO(float, fator_encoder_direito, 1000, 500, 2000) \
    CAMPO(uint32_t, pulsos_encoder_esquerdo_por_giro, 1, 1, 100000) \
    CAMPO(uint32_t, pulsos_encoder_direito_por_giro, 1, 1, 100000) \
    CAMPO(float, fator_tensao_ina228, 1000, 100, 10000) \
    CAMPO(float, fator_corrente_ina228, 1000, 100, 10000) \
    CAMPO(float, fator_aceleracao_mpu, 1000, 500, 2000) \
    CAMPO(float, ganho_pedal, 1000, 500, 2000) \
    CAMPO(uint16_t, limite_acelerador_decimos, 1, 0, 1000) \
    CAMPO(uint32_t, limite_velocidade_centesimos, 1, 0, 3500) \
    CAMPO(uint8_t, marcha_maxima, 1, 1, 3)

#define AJUSTES_DESENVOLVEDOR_EXTRAS(CAMPO) \
    CAMPO(uint8_t, soc_baterias_serie, 1, 1, 5) \
    CAMPO(uint16_t, soc_vazio_mv, 1, 9000, 14000) \
    CAMPO(uint16_t, soc_cheio_mv, 1, 10000, 15000) \
    CAMPO(uint32_t, soc_repouso_ms, 1, 100, 120000) \
    CAMPO(uint8_t, buzzer_intensidade_percentual, 1, 0, 100) \
    CAMPO(uint8_t, led_integrado_habilitado, 1, 0, 1)

#define AJUSTES_DESENVOLVEDOR(CAMPO) \
    AJUSTES_DESENVOLVEDOR_BASE(CAMPO) \
    AJUSTES_DESENVOLVEDOR_EXTRAS(CAMPO)

#define AJUSTE_ID(tipo, nome, escala, minimo, maximo) AJUSTE_##nome,
enum { AJUSTES_DESENVOLVEDOR(AJUSTE_ID) AJUSTES_DESENVOLVEDOR_TOTAL };
#undef AJUSTE_ID

#define AJUSTE_CAMPO(tipo, nome, escala, minimo, maximo) tipo nome;
typedef struct {
    AJUSTES_DESENVOLVEDOR(AJUSTE_CAMPO)
    uint32_t revisao_geometria;
} ajustes_desenvolvedor_t;
#undef AJUSTE_CAMPO

typedef struct __attribute__((packed)) {
    uint8_t formato;
    uint8_t edicao_permitida;
    uint8_t revisao;
    int32_t valores[AJUSTE_soc_baterias_serie];
} resposta_ajustes_desenvolvedor_t;

typedef struct __attribute__((packed)) {
    int32_t valores[AJUSTES_DESENVOLVEDOR_TOTAL - AJUSTE_soc_baterias_serie];
} resposta_ajustes_desenvolvedor_extras_t;

// Consulta sem carga mantém os cinco ajustes do formato original.
#define AJUSTES_EXTRAS_TAMANHO_LEGADO \
    ((AJUSTE_led_integrado_habilitado - AJUSTE_soc_baterias_serie) * sizeof(int32_t))
#define AJUSTES_EXTRAS_FORMATO_COM_LED 1u
