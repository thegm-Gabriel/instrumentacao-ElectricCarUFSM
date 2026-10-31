// Expõe métricas locais de execução do satélite da equipe.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef struct {
    bool cpu_valida;
    uint16_t cpu_total_decimos_percentual;
    uint16_t cpu_nucleo_0_decimos_percentual;
    uint16_t cpu_nucleo_1_decimos_percentual;
    uint16_t cpu_tarefa_pico_decimos_percentual;
    char cpu_tarefa_pico[configMAX_TASK_NAME_LEN];
    uint32_t tarefas_ativas;
    uint32_t menor_pilha_livre_bytes;
    char tarefa_menor_pilha[configMAX_TASK_NAME_LEN];
    bool temperatura_valida;
    int32_t temperatura_centesimos_c;
    esp_err_t temperatura_ultimo_erro;
    uint32_t heap_interno_total;
    uint32_t heap_interno_livre;
    uint32_t heap_interno_minimo;
    uint32_t heap_interno_maior_bloco;
    uint32_t psram_total;
    uint32_t psram_livre;
    uint32_t psram_minima;
    uint32_t psram_maior_bloco;
    uint32_t dma_livre;
    uint32_t dma_maior_bloco;
} diagnostico_sistema_t;

// A temperatura é opcional: uma falha no sensor interno não bloqueia o painel.
esp_err_t diagnostico_sistema_iniciar(void);
diagnostico_sistema_t diagnostico_sistema_obter(void);
