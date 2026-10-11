// Define quadros, códigos e validações do protocolo de comandos.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "protocolo_crc32.h"

#define COMANDO_INICIO_1 0xC7u
#define COMANDO_INICIO_2 0x7Cu
#define COMANDO_VERSAO_PROTOCOLO 1u
#define COMANDO_TAMANHO_MAXIMO_CARGA 128u
#define COMANDO_FORMATO_OTA_DETALHADO 2u

#define CAPACIDADE_COMANDO_PING (1u << 0)
#define CAPACIDADE_COMANDO_VERSAO (1u << 1)
#define CAPACIDADE_COMANDO_SAUDE (1u << 2)
#define CAPACIDADE_COMANDO_TELEMETRIA (1u << 3)
#define CAPACIDADE_COMANDO_UART (1u << 4)
#define CAPACIDADE_COMANDO_OTA (1u << 5)
#define CAPACIDADE_COMANDO_WIFI (1u << 6)
#define CAPACIDADE_COMANDO_CONTROLE_OTA (1u << 7)
#define CAPACIDADE_COMANDO_TERMINAL (1u << 8)
#define CAPACIDADE_COMANDO_CONTROLE_OPERACIONAL (1u << 9)

typedef enum {
    NO_COMANDO_DESCONHECIDO = 0,
    NO_COMANDO_MESTRE = 1,
    NO_COMANDO_EQUIPE = 2,
    NO_COMANDO_VISITANTES = 3,
} no_protocolo_comando_t;

typedef enum {
    TIPO_QUADRO_COMANDO_SOLICITACAO = 1,
    TIPO_QUADRO_COMANDO_RESPOSTA = 2,
} tipo_quadro_comando_t;

typedef enum {
    COMANDO_PING = 1,
    COMANDO_OBTER_VERSAO = 2,
    COMANDO_OBTER_CAPACIDADES = 3,
    COMANDO_OBTER_SAUDE = 4,
    COMANDO_OBTER_TELEMETRIA = 5,
    COMANDO_OBTER_ESTADO_UART = 6,
    COMANDO_OBTER_ESTADO_OTA = 7,
    COMANDO_OBTER_ESTADO_WIFI = 8,
    COMANDO_SOLICITAR_VERIFICACAO_OTA = 20,
    COMANDO_AUTORIZAR_OTA_MESTRE = 21,
    COMANDO_AUTORIZAR_OTA_EQUIPE = 22,
    COMANDO_AUTORIZAR_OTA_VISITANTES = 23,
    COMANDO_CANCELAR_OTA = 24,
    COMANDO_AUTORIZAR_OTA_TODOS = 25,
    COMANDO_SOLICITAR_RESUMO_TERMINAL = 30,
    COMANDO_EXECUTAR_ACAO_MESTRE = 31,
    COMANDO_OBTER_CONFIGURACAO_MESTRE = 32,
    COMANDO_OBTER_REDE_WIFI_SALVA = 33,
    COMANDO_CONSULTAR_SATELITE = 34,
} codigo_comando_t;

// Uma única ação estruturada transporta as operações do menu serial sem
// reproduzir na UART o estado interativo do terminal físico.
typedef enum {
    ACAO_MESTRE_ALTERNAR_RELATORIOS = 1,
    ACAO_MESTRE_ZERAR_PERCURSO = 2,
    ACAO_MESTRE_SINCRONIZAR_GOOGLE = 3,
    ACAO_MESTRE_DEFINIR_INTERVALO_RELATORIO = 4,
    ACAO_MESTRE_DEFINIR_INTERVALO_OTA = 5,
    ACAO_MESTRE_DEFINIR_INTERVALO_TELEMETRIA_OTA = 6,
    ACAO_MESTRE_PREPARAR_REDE_WIFI = 7,
    ACAO_MESTRE_CONECTAR_REDE_WIFI_PREPARADA = 8,
    ACAO_MESTRE_SALVAR_REDE_WIFI_PREPARADA = 9,
    ACAO_MESTRE_CONECTAR_REDE_WIFI_SALVA = 10,
    ACAO_MESTRE_EXCLUIR_REDE_WIFI_SALVA = 11,
    ACAO_MESTRE_DEFINIR_WIFI_AUTOMATICO = 12,
    ACAO_MESTRE_ALTERAR_REDE_WIFI_PRINCIPAL = 13,
    ACAO_MESTRE_RESTAURAR_REDE_WIFI_PRINCIPAL = 14,
    ACAO_MESTRE_DEFINIR_POLITICA_WIFI = 15,
} acao_mestre_t;

typedef enum {
    RESPOSTA_COMANDO_OK = 0,
    RESPOSTA_COMANDO_NAO_SUPORTADO = 1,
    RESPOSTA_COMANDO_CARGA_INVALIDA = 2,
    RESPOSTA_COMANDO_OCUPADO = 3,
    RESPOSTA_COMANDO_NEGADO = 4,
    RESPOSTA_COMANDO_ERRO_INTERNO = 5,
    RESPOSTA_COMANDO_TIMEOUT = 6,
} codigo_resposta_comando_t;

typedef struct __attribute__((packed)) {
    uint8_t inicio_1;
    uint8_t inicio_2;
    uint8_t versao_protocolo;
    uint8_t tipo;
    uint8_t origem;
    uint8_t destino;
    uint16_t comando;
    uint32_t solicitacao;
    uint16_t tamanho_carga;
    uint8_t resultado;
    uint8_t reservado;
} cabecalho_quadro_comando_t;

typedef struct __attribute__((packed)) {
    uint32_t tempo_ativo_ms;
} resposta_comando_ping_t;

typedef struct __attribute__((packed)) {
    char versao[32];
    uint32_t capacidades;
    uint8_t no;
} resposta_comando_versao_t;

typedef struct __attribute__((packed)) {
    uint32_t capacidades;
} resposta_comando_capacidades_t;

typedef struct __attribute__((packed)) {
    uint8_t estado;
    uint32_t causas_ativas;
    uint32_t tempo_ativo_ms;
    uint32_t heap_livre;
    uint32_t erros_historicos;
} resposta_comando_saude_t;

typedef struct __attribute__((packed)) {
    uint16_t sequencia;
    uint32_t tempo_mestre_ms;
    uint16_t tensao_adc_bruta;
    int16_t aceleracao_x;
    int16_t aceleracao_y;
    int16_t aceleracao_z;
    int16_t giroscopio_x;
    int16_t giroscopio_y;
    int16_t giroscopio_z;
    uint16_t velocidade_centesimos_kmh;
    uint16_t carga_decimos_percentual;
} resposta_comando_telemetria_t;

typedef struct __attribute__((packed)) {
    uint32_t envios;
    uint32_t recepcoes;
    uint32_t erros;
    uint32_t descartes;
} resposta_comando_uart_t;

typedef struct __attribute__((packed)) {
    uint8_t estado;
    uint8_t alvo;
    uint16_t progresso_decimos;
    uint32_t falhas;
    char versao_atual[32];
    char versao_disponivel[32];
} resposta_comando_ota_t;

// Formato versionado usado para apresentar cada alvo OTA separadamente. A
// carga antiga acima continua válida e pode ser reconhecida pelo seu tamanho.
typedef struct __attribute__((packed)) {
    uint8_t versao_formato;
    uint8_t estado;
    uint8_t alvo_ativo;
    uint8_t estado_equipe;
    uint8_t estado_visitantes;
    uint8_t estado_mestre;
    uint16_t progresso_mestre_decimos;
    uint16_t progresso_equipe_decimos;
    uint16_t progresso_visitantes_decimos;
    uint32_t falhas;
    uint32_t verificacoes;
    char versao_mestre_atual[18];
    char versao_mestre_disponivel[18];
    char versao_equipe_atual[18];
    char versao_equipe_disponivel[18];
    char versao_visitantes_atual[18];
    char versao_visitantes_disponivel[18];
} resposta_comando_ota_detalhada_t;

typedef struct __attribute__((packed)) {
    uint8_t radio_ativo;
    uint8_t conectado;
    int8_t rssi_dbm;
    uint8_t canal;
    char rede[33];
} resposta_comando_wifi_t;

typedef struct __attribute__((packed)) {
    uint8_t acao;
    uint8_t alvo;
    uint16_t reservado;
    uint32_t valor;
    uint32_t valor_2;
    uint32_t valor_3;
    uint32_t valor_4;
    char texto[108];
} solicitacao_acao_mestre_t;

typedef struct __attribute__((packed)) {
    uint8_t relatorios_pausados;
    uint8_t quantidade_redes_wifi;
    uint8_t wifi_automatico;
    uint8_t cartao_disponivel;
    uint32_t intervalos_relatorios_ms[4];
    uint32_t intervalo_ota_minutos;
    uint32_t intervalo_telemetria_ota_ms;
    uint32_t amostras_sd;
    uint32_t arquivos_pendentes;
    uint32_t arquivos_enviados;
    uint8_t tentativas_wifi;
    uint8_t reservado[3];
    uint32_t tempo_tentativa_wifi_ms;
    uint32_t intervalo_tentativas_wifi_ms;
    uint32_t tempo_limite_wifi_ms;
} resposta_configuracao_mestre_t;

typedef struct __attribute__((packed)) {
    uint8_t indice;
} solicitacao_rede_wifi_salva_t;

typedef struct __attribute__((packed)) {
    uint8_t indice;
    char ssid[33];
} resposta_rede_wifi_salva_t;

typedef enum {
    CONSULTA_SATELITE_PING = 1,
    CONSULTA_SATELITE_VERSAO = 2,
} tipo_consulta_satelite_t;

typedef struct __attribute__((packed)) {
    uint8_t alvo;
    uint8_t tipo;
    uint16_t reservado;
} solicitacao_consulta_satelite_t;

typedef struct __attribute__((packed)) {
    uint8_t alvo;
    uint8_t tipo;
    uint16_t reservado;
    uint32_t rtt_ms;
    uint32_t tempo_ativo_ms;
    uint32_t capacidades;
    char versao[32];
} resposta_consulta_satelite_t;

// Confirma que uma ação com efeito colateral foi aceita pelo mestre. O efeito
// continua de forma assíncrona e deve ser acompanhado pela consulta de estado.
typedef struct __attribute__((packed)) {
    uint16_t comando;
    uint8_t aceito;
    uint8_t reservado;
    uint32_t tempo_mestre_ms;
} resposta_comando_acao_t;

#define COMANDO_TAMANHO_CABECALHO ((size_t)sizeof(cabecalho_quadro_comando_t))
#define COMANDO_TAMANHO_CRC ((size_t)sizeof(uint32_t))
#define COMANDO_TAMANHO_MAXIMO_QUADRO \
    (COMANDO_TAMANHO_CABECALHO + COMANDO_TAMANHO_MAXIMO_CARGA + COMANDO_TAMANHO_CRC)

#ifdef __cplusplus
static_assert(sizeof(cabecalho_quadro_comando_t) == 16,
              "O cabeçalho de comando deve ter 16 bytes");
static_assert(sizeof(resposta_comando_ota_t) <= COMANDO_TAMANHO_MAXIMO_CARGA,
              "As respostas devem caber na carga de comando");
static_assert(sizeof(resposta_comando_ota_detalhada_t) ==
                  COMANDO_TAMANHO_MAXIMO_CARGA,
              "A resposta OTA detalhada deve ocupar 128 bytes");
static_assert(sizeof(resposta_comando_acao_t) == 8,
              "A confirmação de ação deve ter 8 bytes");
static_assert(sizeof(solicitacao_acao_mestre_t) ==
                  COMANDO_TAMANHO_MAXIMO_CARGA,
              "A solicitação operacional deve ocupar 128 bytes");
static_assert(sizeof(resposta_configuracao_mestre_t) <=
                  COMANDO_TAMANHO_MAXIMO_CARGA,
              "A configuração deve caber na carga de comando");
#else
_Static_assert(sizeof(cabecalho_quadro_comando_t) == 16,
               "O cabeçalho de comando deve ter 16 bytes");
_Static_assert(sizeof(resposta_comando_ota_t) <= COMANDO_TAMANHO_MAXIMO_CARGA,
               "As respostas devem caber na carga de comando");
_Static_assert(sizeof(resposta_comando_ota_detalhada_t) ==
                   COMANDO_TAMANHO_MAXIMO_CARGA,
               "A resposta OTA detalhada deve ocupar 128 bytes");
_Static_assert(sizeof(resposta_comando_acao_t) == 8,
               "A confirmação de ação deve ter 8 bytes");
_Static_assert(sizeof(solicitacao_acao_mestre_t) ==
                   COMANDO_TAMANHO_MAXIMO_CARGA,
               "A solicitação operacional deve ocupar 128 bytes");
_Static_assert(sizeof(resposta_configuracao_mestre_t) <=
                   COMANDO_TAMANHO_MAXIMO_CARGA,
               "A configuração deve caber na carga de comando");
#endif

static inline int protocolo_comandos_no_valido(uint8_t no)
{
    return no >= NO_COMANDO_MESTRE && no <= NO_COMANDO_VISITANTES;
}

static inline size_t protocolo_comandos_tamanho_quadro(uint16_t tamanho_carga)
{
    return COMANDO_TAMANHO_CABECALHO + tamanho_carga + COMANDO_TAMANHO_CRC;
}

static inline int protocolo_comandos_validar_quadro(const uint8_t *quadro,
                                                     size_t tamanho)
{
    if (quadro == NULL ||
        tamanho < COMANDO_TAMANHO_CABECALHO + COMANDO_TAMANHO_CRC) return 0;
    cabecalho_quadro_comando_t cabecalho;
    memcpy(&cabecalho, quadro, sizeof(cabecalho));
    if (cabecalho.inicio_1 != COMANDO_INICIO_1 ||
        cabecalho.inicio_2 != COMANDO_INICIO_2 ||
        cabecalho.versao_protocolo != COMANDO_VERSAO_PROTOCOLO ||
        (cabecalho.tipo != TIPO_QUADRO_COMANDO_SOLICITACAO &&
         cabecalho.tipo != TIPO_QUADRO_COMANDO_RESPOSTA) ||
        !protocolo_comandos_no_valido(cabecalho.origem) ||
        !protocolo_comandos_no_valido(cabecalho.destino) ||
        cabecalho.origem == cabecalho.destino ||
        cabecalho.comando == 0 || cabecalho.solicitacao == 0 ||
        cabecalho.reservado != 0 ||
        cabecalho.tamanho_carga > COMANDO_TAMANHO_MAXIMO_CARGA ||
        tamanho != protocolo_comandos_tamanho_quadro(cabecalho.tamanho_carga)) return 0;
    if (cabecalho.tipo == TIPO_QUADRO_COMANDO_SOLICITACAO &&
        cabecalho.resultado != RESPOSTA_COMANDO_OK) return 0;
    if (cabecalho.tipo == TIPO_QUADRO_COMANDO_RESPOSTA &&
        cabecalho.resultado > RESPOSTA_COMANDO_TIMEOUT) return 0;
    uint32_t recebido = 0;
    memcpy(&recebido, quadro + tamanho - COMANDO_TAMANHO_CRC, sizeof(recebido));
    return recebido == protocolo_calcular_crc32(
                           quadro, tamanho - COMANDO_TAMANHO_CRC);
}

static inline size_t protocolo_comandos_montar_quadro(
    uint8_t *destino, size_t capacidade, uint8_t tipo, uint8_t origem,
    uint8_t no_destino, uint16_t comando, uint32_t solicitacao,
    uint8_t resultado, const void *carga, uint16_t tamanho_carga)
{
    if (destino == NULL ||
        (tipo != TIPO_QUADRO_COMANDO_SOLICITACAO &&
         tipo != TIPO_QUADRO_COMANDO_RESPOSTA) ||
        !protocolo_comandos_no_valido(origem) ||
        !protocolo_comandos_no_valido(no_destino) || origem == no_destino ||
        comando == 0 || solicitacao == 0 ||
        tamanho_carga > COMANDO_TAMANHO_MAXIMO_CARGA ||
        (tamanho_carga != 0 && carga == NULL) ||
        (tipo == TIPO_QUADRO_COMANDO_SOLICITACAO &&
         resultado != RESPOSTA_COMANDO_OK)) return 0;
    const size_t tamanho = protocolo_comandos_tamanho_quadro(tamanho_carga);
    if (capacidade < tamanho) return 0;
    const cabecalho_quadro_comando_t cabecalho = {
        COMANDO_INICIO_1, COMANDO_INICIO_2, COMANDO_VERSAO_PROTOCOLO, tipo,
        origem, no_destino, comando, solicitacao, tamanho_carga, resultado, 0};
    memcpy(destino, &cabecalho, sizeof(cabecalho));
    if (tamanho_carga != 0)
        memcpy(destino + sizeof(cabecalho), carga, tamanho_carga);
    const uint32_t crc = protocolo_calcular_crc32(
        destino, tamanho - COMANDO_TAMANHO_CRC);
    memcpy(destino + tamanho - COMANDO_TAMANHO_CRC, &crc, sizeof(crc));
    return tamanho;
}
