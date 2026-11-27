// Configura a UART e executa as operações de recepção e transmissão.
#include "gerenciadores/gerenciador_uart.h"

#include <limits.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "nucleo/configuracao.h"

static const char *TAG = "gerenciador_uart";

esp_err_t gerenciador_uart_iniciar(void)
{
    const uart_config_t configuracao = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t erro = uart_driver_install(
        UART_TELEMETRIA, UART_TAMANHO_BUFFER, 0, 0, NULL, 0);
    if (erro != ESP_OK) return erro;

    erro = uart_param_config(UART_TELEMETRIA, &configuracao);
    if (erro == ESP_OK) {
        erro = uart_set_pin(UART_TELEMETRIA, PINO_UART_TX, PINO_UART_RX,
                            UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (erro != ESP_OK) {
        (void)uart_driver_delete(UART_TELEMETRIA);
        return erro;
    }

    ESP_LOGI(TAG, "UART visitantes: RX GPIO %d, TX GPIO %d, %d baud",
             PINO_UART_RX, PINO_UART_TX, UART_BAUD_RATE);
    return ESP_OK;
}

int gerenciador_uart_receber(void *buffer, size_t capacidade, TickType_t tempo_limite)
{
    if (buffer == NULL || capacidade == 0) return -1;
    int recebidos = uart_read_bytes(UART_TELEMETRIA, buffer, 1, tempo_limite);
    if (recebidos == 1 && capacidade > 1) {
        const int disponiveis = uart_read_bytes(
            UART_TELEMETRIA, (uint8_t *)buffer + 1, capacidade - 1, 0);
        if (disponiveis > 0) recebidos += disponiveis;
    }
    return recebidos;
}

int gerenciador_uart_escrever(const void *dados, size_t tamanho)
{
    if (dados == NULL || tamanho == 0 || tamanho > INT_MAX) return -1;
    return uart_write_bytes(UART_TELEMETRIA, dados, tamanho);
}

esp_err_t gerenciador_uart_enviar(const void *dados, size_t tamanho)
{
    const int escritos = gerenciador_uart_escrever(dados, tamanho);
    if (escritos < 0 || (size_t)escritos != tamanho) return ESP_FAIL;
    return uart_wait_tx_done(UART_TELEMETRIA, pdMS_TO_TICKS(UART_TEMPO_LIMITE_TX_MS));
}
