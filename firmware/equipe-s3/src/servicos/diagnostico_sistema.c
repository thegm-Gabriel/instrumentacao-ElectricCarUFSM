// Reúne CPU, pilhas, memória e temperatura sem acoplar a coleta ao servidor HTTP.
#include "servicos/diagnostico_sistema.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "driver/temperature_sensor.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MAXIMO_TAREFAS_DIAGNOSTICO 32

static const char *TAG = "diagnostico_equipe";
static temperature_sensor_handle_t sensor_temperatura;
static esp_err_t temperatura_ultimo_erro = ESP_ERR_INVALID_STATE;
static TaskStatus_t tarefas[MAXIMO_TAREFAS_DIAGNOSTICO];
static configRUN_TIME_COUNTER_TYPE tempos_anteriores[MAXIMO_TAREFAS_DIAGNOSTICO];
static UBaseType_t ids_anteriores[MAXIMO_TAREFAS_DIAGNOSTICO];
static UBaseType_t quantidade_anterior;
static int64_t coleta_anterior_us;
static uint32_t menor_heap_interno = UINT32_MAX;
static uint32_t menor_psram = UINT32_MAX;

static configRUN_TIME_COUNTER_TYPE tempo_anterior_da_tarefa(
    UBaseType_t identificador, bool *encontrada)
{
    for (UBaseType_t i = 0; i < quantidade_anterior; ++i) {
        if (ids_anteriores[i] == identificador) {
            *encontrada = true;
            return tempos_anteriores[i];
        }
    }
    *encontrada = false;
    return 0;
}

static uint16_t limitar_decimos_percentual(uint64_t valor)
{
    return (uint16_t)(valor > 1000U ? 1000U : valor);
}

esp_err_t diagnostico_sistema_iniciar(void)
{
    if (sensor_temperatura != NULL) return ESP_OK;
    const temperature_sensor_config_t configuracao =
        TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    temperatura_ultimo_erro =
        temperature_sensor_install(&configuracao, &sensor_temperatura);
    if (temperatura_ultimo_erro == ESP_OK) {
        temperatura_ultimo_erro = temperature_sensor_enable(sensor_temperatura);
    }
    if (temperatura_ultimo_erro != ESP_OK) {
        if (sensor_temperatura != NULL) {
            (void)temperature_sensor_uninstall(sensor_temperatura);
            sensor_temperatura = NULL;
        }
        ESP_LOGW(TAG, "Temperatura interna indisponível: %s",
                 esp_err_to_name(temperatura_ultimo_erro));
    } else {
        ESP_LOGI(TAG, "Diagnóstico de CPU, memória, pilhas e temperatura ativo");
    }
    return temperatura_ultimo_erro;
}

diagnostico_sistema_t diagnostico_sistema_obter(void)
{
    diagnostico_sistema_t resultado = {0};
    resultado.heap_interno_total = heap_caps_get_total_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    resultado.heap_interno_livre = heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    resultado.heap_interno_maior_bloco = heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (resultado.heap_interno_livre < menor_heap_interno) {
        menor_heap_interno = resultado.heap_interno_livre;
    }
    resultado.heap_interno_minimo = menor_heap_interno;
    resultado.psram_total = heap_caps_get_total_size(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    resultado.psram_livre = heap_caps_get_free_size(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    resultado.psram_maior_bloco = heap_caps_get_largest_free_block(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (resultado.psram_total != 0 && resultado.psram_livre < menor_psram) {
        menor_psram = resultado.psram_livre;
    }
    resultado.psram_minima = resultado.psram_total == 0 ? 0 : menor_psram;
    resultado.dma_livre = heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    resultado.dma_maior_bloco = heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    resultado.temperatura_ultimo_erro = temperatura_ultimo_erro;
    if (sensor_temperatura != NULL) {
        float temperatura = 0.0f;
        temperatura_ultimo_erro =
            temperature_sensor_get_celsius(sensor_temperatura, &temperatura);
        resultado.temperatura_ultimo_erro = temperatura_ultimo_erro;
        if (temperatura_ultimo_erro == ESP_OK) {
            resultado.temperatura_valida = true;
            resultado.temperatura_centesimos_c =
                (int32_t)(temperatura * 100.0f +
                          (temperatura >= 0.0f ? 0.5f : -0.5f));
        }
    }

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    configRUN_TIME_COUNTER_TYPE total = 0;
    const UBaseType_t quantidade = uxTaskGetSystemState(
        tarefas, MAXIMO_TAREFAS_DIAGNOSTICO, &total);
    const int64_t agora_us = esp_timer_get_time();
    resultado.tarefas_ativas = quantidade;
    resultado.menor_pilha_livre_bytes = UINT32_MAX;
    uint64_t uso_nucleo_0 = 0;
    uint64_t uso_nucleo_1 = 0;
    uint64_t maior_uso_tarefa = 0;
    const uint64_t janela_us = coleta_anterior_us != 0 &&
                                agora_us > coleta_anterior_us
                                    ? (uint64_t)(agora_us - coleta_anterior_us)
                                    : 0;
    for (UBaseType_t i = 0; i < quantidade; ++i) {
        const TaskStatus_t *tarefa = &tarefas[i];
        if (tarefa->usStackHighWaterMark < resultado.menor_pilha_livre_bytes) {
            resultado.menor_pilha_livre_bytes = tarefa->usStackHighWaterMark;
            snprintf(resultado.tarefa_menor_pilha,
                     sizeof(resultado.tarefa_menor_pilha), "%s",
                     tarefa->pcTaskName);
        }
        bool encontrada = false;
        const configRUN_TIME_COUNTER_TYPE anterior =
            tempo_anterior_da_tarefa(tarefa->xTaskNumber, &encontrada);
        if (!encontrada || janela_us == 0 ||
            tarefa->ulRunTimeCounter < anterior) continue;
        const uint64_t delta = tarefa->ulRunTimeCounter - anterior;
        if (strcmp(tarefa->pcTaskName, "IDLE0") == 0) {
            uso_nucleo_0 = 1000U - limitar_decimos_percentual(
                delta * 1000U / janela_us);
        } else if (strcmp(tarefa->pcTaskName, "IDLE1") == 0) {
            uso_nucleo_1 = 1000U - limitar_decimos_percentual(
                delta * 1000U / janela_us);
        } else if (delta > maior_uso_tarefa) {
            maior_uso_tarefa = delta;
            snprintf(resultado.cpu_tarefa_pico,
                     sizeof(resultado.cpu_tarefa_pico), "%s",
                     tarefa->pcTaskName);
        }
    }
    if (quantidade != 0 && janela_us != 0) {
        resultado.cpu_valida = true;
        resultado.cpu_nucleo_0_decimos_percentual =
            limitar_decimos_percentual(uso_nucleo_0);
        resultado.cpu_nucleo_1_decimos_percentual =
            limitar_decimos_percentual(uso_nucleo_1);
        resultado.cpu_total_decimos_percentual = (uint16_t)(
            (resultado.cpu_nucleo_0_decimos_percentual +
             resultado.cpu_nucleo_1_decimos_percentual) / 2U);
        resultado.cpu_tarefa_pico_decimos_percentual =
            limitar_decimos_percentual(maior_uso_tarefa * 1000U / janela_us);
    }
    memset(ids_anteriores, 0, sizeof(ids_anteriores));
    memset(tempos_anteriores, 0, sizeof(tempos_anteriores));
    for (UBaseType_t i = 0; i < quantidade; ++i) {
        ids_anteriores[i] = tarefas[i].xTaskNumber;
        tempos_anteriores[i] = tarefas[i].ulRunTimeCounter;
    }
    quantidade_anterior = quantidade;
    coleta_anterior_us = agora_us;
    if (resultado.menor_pilha_livre_bytes == UINT32_MAX) {
        resultado.menor_pilha_livre_bytes = 0;
    }
#endif
    return resultado;
}
