#include "gerenciador_uart.h"

#include <limits.h>

#include "configuracao.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "gerenciador_uart";
static SemaphoreHandle_t mutex_tx;
static SemaphoreHandle_t mutex_rx;
static SemaphoreHandle_t mutex_estatisticas;
static QueueHandle_t fila_eventos;
static estatisticas_gerenciador_uart_t estado;

static void registrar_erro(esp_err_t erro, bool transmissao)
{
    if (mutex_estatisticas == NULL) return;
    xSemaphoreTake(mutex_estatisticas, portMAX_DELAY);
    if (transmissao) {
        estado.erros_envio++;
        estado.ultimo_erro_envio = erro;
    } else {
        estado.erros_recepcao++;
        estado.ultimo_erro_recepcao = erro;
    }
    xSemaphoreGive(mutex_estatisticas);
}

static void tarefa_eventos_uart(void *argumento)
{
    (void)argumento;
    uart_event_t evento;
    while (true) {
        if (xQueueReceive(fila_eventos, &evento, portMAX_DELAY) != pdTRUE) continue;
        if (evento.type == UART_DATA) continue;

        xSemaphoreTake(mutex_estatisticas, portMAX_DELAY);
        switch (evento.type) {
            case UART_FIFO_OVF:
                estado.estouros_fifo++;
                ESP_LOGE(TAG, "Overflow no FIFO de recepcao");
                break;
            case UART_BUFFER_FULL:
                estado.buffers_cheios++;
                ESP_LOGE(TAG, "Buffer de recepcao cheio");
                break;
            case UART_FRAME_ERR:
                estado.erros_quadro++;
                ESP_LOGE(TAG, "Erro de quadro UART; verifique baud rate e sinal eletrico");
                break;
            case UART_PARITY_ERR:
                estado.erros_paridade++;
                ESP_LOGE(TAG, "Erro de paridade UART");
                break;
            case UART_BREAK:
                estado.sinais_break++;
                ESP_LOGW(TAG, "Sinal de break detectado na UART");
                break;
            default:
                ESP_LOGW(TAG, "Evento UART nao tratado: %d", evento.type);
                break;
        }
        estado.erros_recepcao++;
        estado.ultimo_erro_recepcao = ESP_FAIL;
        xSemaphoreGive(mutex_estatisticas);

        if (evento.type == UART_FIFO_OVF || evento.type == UART_BUFFER_FULL) {
            if (xSemaphoreTake(mutex_rx, pdMS_TO_TICKS(10)) == pdTRUE) {
                uart_flush_input(UART_TELEMETRIA);
                xQueueReset(fila_eventos);
                xSemaphoreGive(mutex_rx);
            }
        }
    }
}

esp_err_t gerenciador_uart_iniciar(void)
{
    if (estado.inicializado) return ESP_OK;

    if (mutex_tx == NULL) mutex_tx = xSemaphoreCreateMutex();
    if (mutex_rx == NULL) mutex_rx = xSemaphoreCreateMutex();
    if (mutex_estatisticas == NULL) mutex_estatisticas = xSemaphoreCreateMutex();
    if (mutex_tx == NULL || mutex_rx == NULL || mutex_estatisticas == NULL) {
        ESP_LOGE(TAG, "Memoria insuficiente para os mutexes da UART");
        return ESP_ERR_NO_MEM;
    }

    const uart_config_t configuracao = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t erro = uart_driver_install(UART_TELEMETRIA, UART_TAMANHO_BUFFER_RX,
                                         UART_TAMANHO_BUFFER_TX, 20, &fila_eventos, 0);
    if (erro == ESP_OK) erro = uart_param_config(UART_TELEMETRIA, &configuracao);
    if (erro == ESP_OK) {
        erro = uart_set_pin(UART_TELEMETRIA, PINO_UART_TX, PINO_UART_RX,
                            UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (erro != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao configurar UART: %s", esp_err_to_name(erro));
        uart_driver_delete(UART_TELEMETRIA);
        fila_eventos = NULL;
        registrar_erro(erro, false);
        return erro;
    }

    if (xTaskCreate(tarefa_eventos_uart, "eventos_uart", 3072, NULL, 6, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Nao foi possivel criar a tarefa de eventos UART");
        uart_driver_delete(UART_TELEMETRIA);
        fila_eventos = NULL;
        registrar_erro(ESP_ERR_NO_MEM, false);
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(mutex_estatisticas, portMAX_DELAY);
    estado.inicializado = true;
    estado.ultimo_erro_envio = ESP_OK;
    estado.ultimo_erro_recepcao = ESP_OK;
    xSemaphoreGive(mutex_estatisticas);
    ESP_LOGI(TAG, "UART pronta: TX GPIO %d, RX GPIO %d, %d baud 8N1",
             PINO_UART_TX, PINO_UART_RX, UART_BAUD_RATE);
    return ESP_OK;
}

esp_err_t gerenciador_uart_enviar(const void *dados, size_t tamanho)
{
    return gerenciador_uart_enviar_com_timeout(
        dados, tamanho, pdMS_TO_TICKS(UART_TEMPO_LIMITE_TX_MS));
}

esp_err_t gerenciador_uart_enviar_com_timeout(const void *dados, size_t tamanho,
                                              TickType_t tempo_limite)
{
    if (dados == NULL || tamanho == 0 || tamanho > INT_MAX) {
        ESP_LOGE(TAG, "Envio rejeitado: buffer ou tamanho invalido (%u bytes)",
                 (unsigned)tamanho);
        return ESP_ERR_INVALID_ARG;
    }
    if (!estado.inicializado) {
        ESP_LOGE(TAG, "Tentativa de envio antes da inicializacao");
        registrar_erro(ESP_ERR_INVALID_STATE, true);
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(mutex_tx, tempo_limite) != pdTRUE) {
        ESP_LOGE(TAG, "Timeout aguardando acesso ao transmissor");
        registrar_erro(ESP_ERR_TIMEOUT, true);
        return ESP_ERR_TIMEOUT;
    }

    int escritos = uart_write_bytes(UART_TELEMETRIA, dados, tamanho);
    esp_err_t resultado = ESP_OK;
    if (escritos != (int)tamanho) {
        resultado = ESP_FAIL;
        ESP_LOGE(TAG, "UART aceitou %d de %u bytes", escritos, (unsigned)tamanho);
    } else {
        resultado = uart_wait_tx_done(UART_TELEMETRIA, tempo_limite);
        if (resultado != ESP_OK) {
            ESP_LOGE(TAG, "Transmissao nao concluida: %s", esp_err_to_name(resultado));
        }
    }
    xSemaphoreGive(mutex_tx);

    xSemaphoreTake(mutex_estatisticas, portMAX_DELAY);
    if (resultado == ESP_OK) {
        estado.envios_ok++;
        estado.bytes_enviados += tamanho;
    } else {
        estado.erros_envio++;
        estado.ultimo_erro_envio = resultado;
    }
    xSemaphoreGive(mutex_estatisticas);
    return resultado;
}

int gerenciador_uart_receber(void *buffer, size_t capacidade, TickType_t tempo_limite)
{
    if (buffer == NULL || capacidade == 0 || capacidade > INT_MAX) {
        ESP_LOGE(TAG, "Recepcao rejeitada: buffer ou tamanho invalido");
        return -1;
    }
    if (!estado.inicializado) {
        registrar_erro(ESP_ERR_INVALID_STATE, false);
        return -1;
    }
    if (xSemaphoreTake(mutex_rx, tempo_limite) != pdTRUE) {
        registrar_erro(ESP_ERR_TIMEOUT, false);
        return -1;
    }
    int recebidos = uart_read_bytes(UART_TELEMETRIA, buffer, capacidade, tempo_limite);
    xSemaphoreGive(mutex_rx);

    xSemaphoreTake(mutex_estatisticas, portMAX_DELAY);
    if (recebidos >= 0) {
        if (recebidos > 0) estado.recepcoes_ok++;
        estado.bytes_recebidos += recebidos;
    } else {
        estado.erros_recepcao++;
        estado.ultimo_erro_recepcao = ESP_FAIL;
    }
    xSemaphoreGive(mutex_estatisticas);
    if (recebidos < 0) ESP_LOGE(TAG, "Falha ao ler a UART");
    return recebidos;
}

esp_err_t gerenciador_uart_limpar_recepcao(void)
{
    if (!estado.inicializado) return ESP_ERR_INVALID_STATE;
    esp_err_t erro = uart_flush_input(UART_TELEMETRIA);
    if (erro != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao limpar recepcao: %s", esp_err_to_name(erro));
        registrar_erro(erro, false);
    } else {
        ESP_LOGW(TAG, "Buffer de recepcao descartado");
    }
    return erro;
}

void gerenciador_uart_obter_estatisticas(estatisticas_gerenciador_uart_t *estatisticas)
{
    if (estatisticas == NULL) return;
    if (mutex_estatisticas == NULL) {
        *estatisticas = (estatisticas_gerenciador_uart_t){0};
        return;
    }
    xSemaphoreTake(mutex_estatisticas, portMAX_DELAY);
    *estatisticas = estado;
    xSemaphoreGive(mutex_estatisticas);
}
