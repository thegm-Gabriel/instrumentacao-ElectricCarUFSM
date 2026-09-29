#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "protocolo_comandos.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*funcao_envio_comando_satelite_t)(const void *dados,
                                                       size_t tamanho);

typedef struct {
    const char *nome_satelite;
    no_protocolo_comando_t no_satelite;
    uint32_t capacidades_adicionais;
    funcao_envio_comando_satelite_t enviar;
} configuracao_servico_comandos_satelite_t;

typedef struct {
    uint32_t solicitacoes_recebidas;
    uint32_t respostas_enviadas;
    uint32_t solicitacoes_enviadas;
    uint32_t respostas_recebidas;
    uint32_t repeticoes;
    uint32_t timeouts;
    uint32_t quadros_invalidos;
    esp_err_t ultimo_erro;
} estatisticas_servico_comandos_satelite_t;

typedef struct {
    uint32_t identificador;
    uint32_t duracao_ms;
    uint8_t tentativas;
} informacoes_solicitacao_comando_t;

esp_err_t servico_comandos_satelite_iniciar(
    const configuracao_servico_comandos_satelite_t *configuracao);

void servico_comandos_satelite_processar_quadro(const uint8_t *quadro,
                                                  size_t tamanho);

esp_err_t servico_comandos_satelite_solicitar(
    codigo_comando_t comando, const void *carga, uint16_t tamanho_carga,
    void *resposta, size_t capacidade_resposta, uint16_t *tamanho_resposta,
    codigo_resposta_comando_t *resultado, TickType_t tempo_limite);

// Variante genérica que também informa o identificador usado no protocolo,
// a duração total e quantas tentativas foram necessárias.
esp_err_t servico_comandos_satelite_solicitar_detalhado(
    codigo_comando_t comando, const void *carga, uint16_t tamanho_carga,
    void *resposta, size_t capacidade_resposta, uint16_t *tamanho_resposta,
    codigo_resposta_comando_t *resultado, TickType_t tempo_limite,
    informacoes_solicitacao_comando_t *informacoes);

estatisticas_servico_comandos_satelite_t
servico_comandos_satelite_obter_estatisticas(void);

#ifdef __cplusplus
}
#endif
