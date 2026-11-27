// Declara o serviço de atualização OTA do satélite Visitantes.
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

esp_err_t servico_ota_satelite_iniciar(
    const configuracao_servico_ota_satelite_t *configuracao);
esp_err_t servico_ota_satelite_confirmar_firmware_em_execucao(void);
bool servico_ota_satelite_firmware_em_observacao(void);
esp_err_t servico_ota_satelite_processar_quadro(const uint8_t *quadro,
                                                size_t tamanho);
void servico_ota_satelite_verificar_timeout(void);

#ifdef __cplusplus
}
#endif
