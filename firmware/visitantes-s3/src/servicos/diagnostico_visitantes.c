#include "servicos/diagnostico_visitantes.h"

#include <limits.h>
#include <stdbool.h>
#include <string.h>

#include "driver/temperature_sensor.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gerenciadores/gerenciador_wifi.h"

#define MAXIMO_TAREFAS 32

static temperature_sensor_handle_t sensor;
static esp_err_t erro_temperatura = ESP_ERR_INVALID_STATE;
static bool sensor_consultado;
static uint64_t tempo_idle_anterior;
static int64_t instante_anterior_us;

static uint16_t limitar_uso(uint64_t valor)
{
    return (uint16_t)(valor > 1000U ? 1000U : valor);
}

esp_err_t diagnostico_visitantes_obter(resposta_diagnostico_satelite_t *saida)
{
    if (saida == NULL) return ESP_ERR_INVALID_ARG;
    memset(saida, 0, sizeof(*saida));
    saida->versao_formato = COMANDO_FORMATO_DIAGNOSTICO_SATELITE;
    saida->tempo_ativo_ms = (uint32_t)(esp_timer_get_time() / 1000);
    saida->heap_interno_livre = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    saida->heap_interno_minimo = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    saida->motivo_reinicio = (uint8_t)esp_reset_reason();
    (void)gerenciador_wifi_obter_total_clientes(&saida->clientes_wifi);

    if (!sensor_consultado) {
        sensor_consultado = true;
        const temperature_sensor_config_t config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        erro_temperatura = temperature_sensor_install(&config, &sensor);
        if (erro_temperatura == ESP_OK)
            erro_temperatura = temperature_sensor_enable(sensor);
        if (erro_temperatura != ESP_OK && sensor != NULL) {
            (void)temperature_sensor_uninstall(sensor);
            sensor = NULL;
        }
    }
    if (sensor != NULL) {
        float celsius = 0;
        erro_temperatura = temperature_sensor_get_celsius(sensor, &celsius);
        if (erro_temperatura == ESP_OK) {
            saida->indicadores |= 2U;
            saida->temperatura_centesimos_c = (int16_t)(celsius * 100.0f);
        }
    }
    saida->temperatura_ultimo_erro = erro_temperatura;

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    TaskStatus_t tarefas[MAXIMO_TAREFAS];
    configRUN_TIME_COUNTER_TYPE total = 0;
    const UBaseType_t total_tarefas = uxTaskGetNumberOfTasks();
    const UBaseType_t quantidade = uxTaskGetSystemState(tarefas, MAXIMO_TAREFAS, &total);
    const bool tarefas_truncadas = total_tarefas > MAXIMO_TAREFAS ||
                                    (quantidade == 0 && total_tarefas != 0);
    if (tarefas_truncadas) saida->indicadores |= 4U;
    const int64_t agora_us = esp_timer_get_time();
    const TaskHandle_t tarefa_idle = xTaskGetIdleTaskHandle();
    uint64_t idle_atual = 0;
    bool idle_encontrado = false;
    saida->menor_pilha_livre_bytes = UINT32_MAX;
    saida->tarefas_ativas = total_tarefas > UINT8_MAX ? UINT8_MAX : (uint8_t)total_tarefas;
    for (UBaseType_t i = 0; i < quantidade; ++i) {
        if (tarefas[i].xHandle == tarefa_idle) {
            idle_atual = tarefas[i].ulRunTimeCounter;
            idle_encontrado = true;
        }
        if (tarefas[i].usStackHighWaterMark < saida->menor_pilha_livre_bytes)
            saida->menor_pilha_livre_bytes = tarefas[i].usStackHighWaterMark;
    }
    if (quantidade == 0) saida->menor_pilha_livre_bytes = 0;
    if (!tarefas_truncadas && idle_encontrado && instante_anterior_us > 0 &&
        agora_us > instante_anterior_us && idle_atual >= tempo_idle_anterior) {
        const uint64_t janela = (uint64_t)(agora_us - instante_anterior_us);
        saida->cpu_nucleo_0_decimos_percentual = 1000U - limitar_uso(
            (idle_atual - tempo_idle_anterior) * 1000U / janela);
        // O quadro mantém o campo do núcleo 1 em zero por compatibilidade.
        saida->cpu_total_decimos_percentual = saida->cpu_nucleo_0_decimos_percentual;
        saida->indicadores |= 1U;
    }
    tempo_idle_anterior = idle_atual;
    instante_anterior_us = tarefas_truncadas || !idle_encontrado ? 0 : agora_us;
#endif
    return ESP_OK;
}
