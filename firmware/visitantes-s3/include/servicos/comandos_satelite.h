// Declara o atendimento de comandos do Mestre no satélite Visitantes.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
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
    esp_err_t (*obter_diagnostico)(resposta_diagnostico_satelite_t *saida);
    esp_err_t (*obter_clientes_wifi)(uint8_t inicio, resposta_clientes_wifi_satelite_t *saida);
    esp_err_t (*gerir_wifi)(const solicitacao_gestao_wifi_satelite_t *solicitacao,
                           resposta_gestao_wifi_satelite_t *resposta);
} configuracao_servico_comandos_satelite_t;

esp_err_t servico_comandos_satelite_iniciar(
    const configuracao_servico_comandos_satelite_t *configuracao);

void servico_comandos_satelite_processar_quadro(const uint8_t *quadro,
                                               size_t tamanho);

#ifdef __cplusplus
}
#endif
