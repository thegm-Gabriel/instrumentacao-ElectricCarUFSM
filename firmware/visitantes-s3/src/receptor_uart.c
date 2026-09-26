#include "receptor_uart.h"

#include <stdio.h>
#include <string.h>

#include "configuracao.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "uart_visitantes";
static dados_telemetria_t dados_compartilhados;
static SemaphoreHandle_t mutex_dados;

static void salvar_pacote(const pacote_telemetria_t *pacote, bool valido)
{
    xSemaphoreTake(mutex_dados, portMAX_DELAY);
    if (valido) {
        dados_compartilhados.valido = true;
        dados_compartilhados.sequencia = pacote->sequencia;
        dados_compartilhados.tempo_mestre_ms = pacote->tempo_mestre_ms;
        dados_compartilhados.tensao_adc_bruta = pacote->tensao_adc_bruta;
        dados_compartilhados.aceleracao_x = pacote->aceleracao_x;
        dados_compartilhados.aceleracao_y = pacote->aceleracao_y;
        dados_compartilhados.aceleracao_z = pacote->aceleracao_z;
        dados_compartilhados.giroscopio_x = pacote->giroscopio_x;
        dados_compartilhados.giroscopio_y = pacote->giroscopio_y;
        dados_compartilhados.giroscopio_z = pacote->giroscopio_z;
        dados_compartilhados.pacotes_validos++;
        dados_compartilhados.ultimo_recebimento_ms = (uint32_t)(esp_timer_get_time() / 1000);
    } else {
        dados_compartilhados.pacotes_invalidos++;
    }
    xSemaphoreGive(mutex_dados);
}

static void registrar_dados_recebidos(void)
{
    static int64_t ultimo_log_us;
    if (esp_timer_get_time() - ultimo_log_us < 1000000) return;
    ultimo_log_us = esp_timer_get_time();
    dados_telemetria_t dados;
    receptor_uart_obter_dados(&dados);
    ESP_LOGI(TAG, "RX seq=%u t=%lu ms ADC=%u | acc=[%d, %d, %d] giro=[%d, %d, %d] | ok=%lu erro=%lu",
             dados.sequencia, (unsigned long)dados.tempo_mestre_ms, dados.tensao_adc_bruta,
             dados.aceleracao_x, dados.aceleracao_y, dados.aceleracao_z,
             dados.giroscopio_x, dados.giroscopio_y, dados.giroscopio_z,
             (unsigned long)dados.pacotes_validos, (unsigned long)dados.pacotes_invalidos);
}

static void registrar_pacote_invalido(const uint8_t *buffer)
{
    static int64_t ultimo_log_us;
    if (esp_timer_get_time() - ultimo_log_us < 1000000) return;
    ultimo_log_us = esp_timer_get_time();
    char bytes[3 * TAMANHO_PACOTE_TELEMETRIA + 1];
    size_t posicao = 0;
    for (size_t i = 0; i < TAMANHO_PACOTE_TELEMETRIA; i++)
        posicao += snprintf(&bytes[posicao], sizeof(bytes) - posicao, "%02X ", buffer[i]);
    ESP_LOGW(TAG, "Checksum invalido: %s", bytes);
}

static void tarefa_recepcao_uart(void *argumento)
{
    (void)argumento;
    uint8_t byte, buffer[TAMANHO_PACOTE_TELEMETRIA];
    size_t quantidade = 0, bytes_desde_pausa = 0;
    while (true) {
        if (uart_read_bytes(UART_TELEMETRIA, &byte, 1, pdMS_TO_TICKS(100)) != 1) continue;
        if (++bytes_desde_pausa >= 64) {
            bytes_desde_pausa = 0;
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        if (quantidade == 0) {
            if (byte == TELEMETRIA_INICIO_1) buffer[quantidade++] = byte;
            continue;
        }
        if (quantidade == 1) {
            if (byte == TELEMETRIA_INICIO_2) buffer[quantidade++] = byte;
            else if (byte == TELEMETRIA_INICIO_1) buffer[0] = byte;
            else quantidade = 0;
            continue;
        }
        buffer[quantidade++] = byte;
        if (quantidade != sizeof(buffer)) continue;
        pacote_telemetria_t pacote;
        memcpy(&pacote, buffer, sizeof(pacote));
        bool valido = protocolo_telemetria_cabecalho_valido(&pacote) &&
                      protocolo_telemetria_calcular_checksum(&pacote) == pacote.checksum;
        salvar_pacote(&pacote, valido);
        if (valido) {
            registrar_dados_recebidos();
            quantidade = 0;
            continue;
        }
        registrar_pacote_invalido(buffer);
        quantidade = 0;
        for (size_t i = 1; i + 1 < sizeof(buffer); i++) {
            if (buffer[i] == TELEMETRIA_INICIO_1 &&
                buffer[i + 1] == TELEMETRIA_INICIO_2) {
                quantidade = sizeof(buffer) - i;
                memmove(buffer, &buffer[i], quantidade);
                break;
            }
        }
    }
}

esp_err_t receptor_uart_iniciar(void)
{
    mutex_dados = xSemaphoreCreateMutex();
    if (mutex_dados == NULL) return ESP_ERR_NO_MEM;
    const uart_config_t configuracao = {.baud_rate = UART_BAUD_RATE, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT};
    ESP_RETURN_ON_ERROR(uart_driver_install(UART_TELEMETRIA, UART_TAMANHO_BUFFER, 0, 0, NULL, 0), TAG, "driver UART");
    ESP_RETURN_ON_ERROR(uart_param_config(UART_TELEMETRIA, &configuracao), TAG, "configuracao UART");
    ESP_RETURN_ON_ERROR(uart_set_pin(UART_TELEMETRIA, PINO_UART_TX, PINO_UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "pinos UART");
    if (xTaskCreate(tarefa_recepcao_uart, "uart_visitantes", 4096, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "UART visitantes: RX GPIO %d, TX GPIO %d, %d baud", PINO_UART_RX, PINO_UART_TX, UART_BAUD_RATE);
    return ESP_OK;
}

void receptor_uart_obter_dados(dados_telemetria_t *dados)
{
    if (dados == NULL || mutex_dados == NULL) return;
    xSemaphoreTake(mutex_dados, portMAX_DELAY);
    *dados = dados_compartilhados;
    xSemaphoreGive(mutex_dados);
}
