#include "receptor_uart.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gerenciador_uart.h"

static const char *TAG = "receptor_uart";
static dados_telemetria_t dados_compartilhados;
static SemaphoreHandle_t mutex_dados;

typedef enum {
    PACOTE_CORRETO,
    PACOTE_CHECKSUM_INVALIDO,
    PACOTE_CONTEUDO_INVALIDO,
} resultado_validacao_t;

static void registrar_pacote_invalido(const uint8_t *buffer, resultado_validacao_t motivo,
                                      uint8_t checksum_calculado)
{
    static int64_t ultimo_log_us;
    int64_t agora_us = esp_timer_get_time();
    if (agora_us - ultimo_log_us < 1000000) return;
    ultimo_log_us = agora_us;

    char bytes[3 * TAMANHO_PACOTE_TELEMETRIA + 1];
    size_t posicao = 0;
    for (size_t i = 0; i < TAMANHO_PACOTE_TELEMETRIA; i++) {
        posicao += snprintf(&bytes[posicao], sizeof(bytes) - posicao, "%02X ", buffer[i]);
    }
    if (motivo == PACOTE_CHECKSUM_INVALIDO) {
        ESP_LOGW(TAG, "Checksum invalido: recebido=0x%02X calculado=0x%02X | %s",
                 buffer[TAMANHO_PACOTE_TELEMETRIA - 1], checksum_calculado, bytes);
    } else {
        ESP_LOGW(TAG, "Conteudo incoerente descartado | %s", bytes);
    }
}

static resultado_validacao_t validar_e_salvar_pacote(const pacote_telemetria_t *pacote,
                                                       uint8_t *checksum_calculado)
{
    *checksum_calculado = protocolo_telemetria_calcular_checksum(pacote);
    xSemaphoreTake(mutex_dados, portMAX_DELAY);

    if (*checksum_calculado != pacote->checksum) {
        dados_compartilhados.pacotes_invalidos++;
        dados_compartilhados.falhas_checksum++;
        xSemaphoreGive(mutex_dados);
        return PACOTE_CHECKSUM_INVALIDO;
    }
    if (!protocolo_telemetria_cabecalho_valido(pacote) ||
        pacote->tensao_adc_bruta > 4095) {
        dados_compartilhados.pacotes_invalidos++;
        dados_compartilhados.falhas_conteudo++;
        xSemaphoreGive(mutex_dados);
        return PACOTE_CONTEUDO_INVALIDO;
    }

    if (dados_compartilhados.pacotes_validos > 0) {
        if (pacote->tempo_mestre_ms < dados_compartilhados.tempo_mestre_ms) {
            dados_compartilhados.reinicios_mestre++;
            ESP_LOGW(TAG, "Reinicio do mestre detectado; sequencia resincronizada");
        } else {
            uint16_t diferenca = (uint16_t)(pacote->sequencia - dados_compartilhados.sequencia);
            if (diferenca == 0) {
                dados_compartilhados.pacotes_duplicados++;
            } else if (diferenca < 0x8000) {
                if (diferenca > 1) dados_compartilhados.pacotes_perdidos += diferenca - 1;
            } else {
                dados_compartilhados.pacotes_invalidos++;
                dados_compartilhados.falhas_conteudo++;
                xSemaphoreGive(mutex_dados);
                return PACOTE_CONTEUDO_INVALIDO;
            }
        }
    }

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
    xSemaphoreGive(mutex_dados);
    return PACOTE_CORRETO;
}

static void registrar_estado(void)
{
    static int64_t ultimo_log_us;
    int64_t agora_us = esp_timer_get_time();
    if (agora_us - ultimo_log_us < 1000000) return;
    ultimo_log_us = agora_us;

    dados_telemetria_t dados;
    estatisticas_gerenciador_uart_t uart;
    receptor_uart_obter_dados(&dados);
    gerenciador_uart_obter_estatisticas(&uart);
    ESP_LOGI(TAG, "RX seq=%u t=%lu ms ADC=%u | acc=[%d,%d,%d] giro=[%d,%d,%d]",
             dados.sequencia, (unsigned long)dados.tempo_mestre_ms, dados.tensao_adc_bruta,
             dados.aceleracao_x, dados.aceleracao_y, dados.aceleracao_z,
             dados.giroscopio_x, dados.giroscopio_y, dados.giroscopio_z);
    ESP_LOGI(TAG,
             "PROTOCOLO ok=%lu invalido=%lu checksum=%lu conteudo=%lu perdidos=%lu duplicados=%lu",
             (unsigned long)dados.pacotes_validos, (unsigned long)dados.pacotes_invalidos,
             (unsigned long)dados.falhas_checksum, (unsigned long)dados.falhas_conteudo,
             (unsigned long)dados.pacotes_perdidos, (unsigned long)dados.pacotes_duplicados);
    ESP_LOGI(TAG, "DRIVER RX=%lu chamadas/%llu bytes erro=%lu | TX=%lu/%llu bytes erro=%lu",
             (unsigned long)uart.recepcoes_ok, (unsigned long long)uart.bytes_recebidos,
             (unsigned long)uart.erros_recepcao, (unsigned long)uart.envios_ok,
             (unsigned long long)uart.bytes_enviados, (unsigned long)uart.erros_envio);
    ESP_LOGI(TAG, "FISICO fifo=%lu buffer=%lu quadro=%lu paridade=%lu break=%lu | ultimos RX=%s TX=%s",
             (unsigned long)uart.estouros_fifo, (unsigned long)uart.buffers_cheios,
             (unsigned long)uart.erros_quadro, (unsigned long)uart.erros_paridade,
             (unsigned long)uart.sinais_break, esp_err_to_name(uart.ultimo_erro_recepcao),
             esp_err_to_name(uart.ultimo_erro_envio));
}

static void reaproveitar_inicio_do_quadro(uint8_t *quadro, size_t *quantidade)
{
    *quantidade = 0;
    for (size_t i = 1; i + 1 < TAMANHO_PACOTE_TELEMETRIA; i++) {
        if (quadro[i] == TELEMETRIA_INICIO_1 &&
            quadro[i + 1] == TELEMETRIA_INICIO_2) {
            *quantidade = TAMANHO_PACOTE_TELEMETRIA - i;
            memmove(quadro, &quadro[i], *quantidade);
            return;
        }
    }
}

static void processar_byte(uint8_t byte, uint8_t *quadro, size_t *quantidade)
{
    if (*quantidade == 0) {
        if (byte == TELEMETRIA_INICIO_1) quadro[(*quantidade)++] = byte;
        return;
    }
    if (*quantidade == 1) {
        if (byte == TELEMETRIA_INICIO_2) quadro[(*quantidade)++] = byte;
        else if (byte == TELEMETRIA_INICIO_1) quadro[0] = byte;
        else *quantidade = 0;
        return;
    }

    quadro[(*quantidade)++] = byte;
    if (*quantidade != TAMANHO_PACOTE_TELEMETRIA) return;

    pacote_telemetria_t pacote;
    uint8_t checksum_calculado;
    memcpy(&pacote, quadro, sizeof(pacote));
    resultado_validacao_t resultado = validar_e_salvar_pacote(&pacote, &checksum_calculado);
    if (resultado == PACOTE_CORRETO) {
        *quantidade = 0;
        registrar_estado();
    } else {
        registrar_pacote_invalido(quadro, resultado, checksum_calculado);
        reaproveitar_inicio_do_quadro(quadro, quantidade);
    }
}

static void tarefa_recepcao_uart(void *argumento)
{
    (void)argumento;
    uint8_t lote[128];
    uint8_t quadro[TAMANHO_PACOTE_TELEMETRIA];
    size_t quantidade = 0;

    while (true) {
        int recebidos = gerenciador_uart_receber(lote, sizeof(lote), pdMS_TO_TICKS(100));
        if (recebidos <= 0) continue;
        for (int i = 0; i < recebidos; i++) processar_byte(lote[i], quadro, &quantidade);
        if (recebidos == (int)sizeof(lote)) vTaskDelay(pdMS_TO_TICKS(1));
    }
}

esp_err_t receptor_uart_iniciar(void)
{
    if (mutex_dados == NULL) mutex_dados = xSemaphoreCreateMutex();
    if (mutex_dados == NULL) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(gerenciador_uart_iniciar(), TAG, "gerenciador UART");
    if (xTaskCreate(tarefa_recepcao_uart, "recepcao_uart", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Nao foi possivel criar a tarefa de recepcao");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Receptor de telemetria pronto e aguardando pacotes");
    return ESP_OK;
}

void receptor_uart_obter_dados(dados_telemetria_t *dados)
{
    if (dados == NULL || mutex_dados == NULL) return;
    xSemaphoreTake(mutex_dados, portMAX_DELAY);
    *dados = dados_compartilhados;
    xSemaphoreGive(mutex_dados);
}
