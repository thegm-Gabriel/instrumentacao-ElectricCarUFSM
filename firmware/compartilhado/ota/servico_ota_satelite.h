#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*funcao_envio_ota_satelite_t)(const void *dados, size_t tamanho);

typedef struct {
    const char *nome_satelite;
    funcao_envio_ota_satelite_t enviar;
    uint32_t tempo_limite_sem_dados_ms;
} configuracao_servico_ota_satelite_t;

typedef struct {
    bool iniciado;
    bool recebendo;
    uint32_t sessao;
    uint32_t tamanho_total;
    uint32_t bytes_recebidos;
    uint32_t quadros_recebidos;
    uint32_t falhas;
    esp_err_t ultimo_erro;
    char versao_destino[24];
} situacao_servico_ota_satelite_t;

esp_err_t servico_ota_satelite_iniciar(
    const configuracao_servico_ota_satelite_t *configuracao);
esp_err_t servico_ota_satelite_confirmar_firmware_em_execucao(void);
esp_err_t servico_ota_satelite_processar_quadro(const uint8_t *quadro,
                                                size_t tamanho);
void servico_ota_satelite_verificar_timeout(void);
situacao_servico_ota_satelite_t servico_ota_satelite_obter_situacao(void);

#ifdef __cplusplus
}
#endif
