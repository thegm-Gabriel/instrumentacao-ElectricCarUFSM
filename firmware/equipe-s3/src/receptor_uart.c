#include "receptor_uart.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "configuracao.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "protocolo_enlace_uart.h"
#include "servico_comandos_satelite.h"
#include "servico_ota_satelite.h"
#include "gerenciador_uart.h"

static const char *TAG = "receptor_uart";
static dados_telemetria_t dados_compartilhados;
static SemaphoreHandle_t mutex_dados;
static int64_t menor_deslocamento_ms;
static bool possui_deslocamento;

typedef enum {
    PACOTE_CORRETO,
    PACOTE_CRC_INVALIDO,
    PACOTE_CABECALHO_INVALIDO,
    PACOTE_CONTEUDO_INVALIDO,
} resultado_validacao_t;

static void atualizar_metricas_comunicacao(const pacote_telemetria_t *pacote,
                                            uint32_t agora_ms, bool reinicio)
{
    if (dados_compartilhados.ultimo_recebimento_ms != 0 && !reinicio) {
        const uint32_t intervalo = agora_ms - dados_compartilhados.ultimo_recebimento_ms;
        if (dados_compartilhados.intervalo_medio_ms == 0) {
            dados_compartilhados.intervalo_medio_ms = intervalo;
        } else {
            const uint32_t media_anterior = dados_compartilhados.intervalo_medio_ms;
            const uint32_t desvio = intervalo > media_anterior
                                        ? intervalo - media_anterior
                                        : media_anterior - intervalo;
            dados_compartilhados.intervalo_medio_ms =
                (media_anterior * 7u + intervalo) / 8u;
            dados_compartilhados.jitter_medio_ms =
                (dados_compartilhados.jitter_medio_ms * 7u + desvio) / 8u;
        }
        if (dados_compartilhados.intervalo_medio_ms > 0) {
            dados_compartilhados.taxa_pacotes_centesimos_hz =
                100000u / dados_compartilhados.intervalo_medio_ms;
        }
    }

    const int64_t deslocamento = (int64_t)agora_ms - (int64_t)pacote->tempo_mestre_ms;
    if (!possui_deslocamento || reinicio || deslocamento < menor_deslocamento_ms) {
        menor_deslocamento_ms = deslocamento;
        possui_deslocamento = true;
    }
    const int64_t latencia = deslocamento - menor_deslocamento_ms;
    dados_compartilhados.latencia_relativa_ms =
        latencia <= 0 ? 0u : (latencia > UINT32_MAX ? UINT32_MAX : (uint32_t)latencia);
}

static void enviar_confirmacao(const pacote_telemetria_t *pacote)
{
    confirmacao_telemetria_t confirmacao = {
        .inicio_1 = TELEMETRIA_INICIO_1,
        .inicio_2 = TELEMETRIA_INICIO_CONFIRMACAO_2,
        .versao = TELEMETRIA_VERSAO_PROTOCOLO,
        .tipo = TELEMETRIA_TIPO_CONFIRMACAO,
        .sequencia = pacote->sequencia,
        .tempo_mestre_ms = pacote->tempo_mestre_ms,
        .tipo_satelite = SATELITE_TELEMETRIA_EQUIPE,
        .versao_maior = 0,
        .versao_menor = 0,
        .versao_correcao = 0,
        .capacidades = CAPACIDADE_SATELITE_PAINEL_WEB |
                       CAPACIDADE_SATELITE_PORTAL_CATIVO |
                       CAPACIDADE_SATELITE_METRICAS_LINK |
                       CAPACIDADE_SATELITE_OTA_UART |
                       CAPACIDADE_SATELITE_COMANDOS,
        .crc16 = 0,
    };
    unsigned maior = 0, menor = 0, correcao = 0;
    if (sscanf(esp_app_get_description()->version, "%u.%u.%u",
               &maior, &menor, &correcao) == 3 &&
        maior <= UINT8_MAX && menor <= UINT8_MAX && correcao <= UINT8_MAX) {
        confirmacao.versao_maior = (uint8_t)maior;
        confirmacao.versao_menor = (uint8_t)menor;
        confirmacao.versao_correcao = (uint8_t)correcao;
    }
    confirmacao.crc16 = protocolo_telemetria_calcular_crc_confirmacao(&confirmacao);
    (void)gerenciador_uart_enviar(&confirmacao, sizeof(confirmacao));
}

static void registrar_pacote_invalido(const uint8_t *buffer, resultado_validacao_t motivo,
                                      uint16_t crc_calculado)
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
    if (motivo == PACOTE_CRC_INVALIDO) {
        pacote_telemetria_t pacote;
        memcpy(&pacote, buffer, sizeof(pacote));
        ESP_LOGW(TAG, "CRC-16 invalido: recebido=0x%04X calculado=0x%04X | %s",
                 (unsigned)pacote.crc16, (unsigned)crc_calculado, bytes);
    } else if (motivo == PACOTE_CABECALHO_INVALIDO) {
        ESP_LOGW(TAG, "Cabecalho incompativel: versao=%u tipo=%u carga=%u | %s",
                 (unsigned)buffer[2], (unsigned)buffer[3], (unsigned)buffer[4], bytes);
    } else {
        ESP_LOGW(TAG, "Conteudo incoerente descartado | %s", bytes);
    }
}

static resultado_validacao_t validar_e_salvar_pacote(const pacote_telemetria_t *pacote,
                                                       uint16_t *crc_calculado)
{
    *crc_calculado = protocolo_telemetria_calcular_crc16(pacote);
    xSemaphoreTake(mutex_dados, portMAX_DELAY);

    if (!protocolo_telemetria_cabecalho_valido(pacote)) {
        dados_compartilhados.pacotes_invalidos++;
        dados_compartilhados.falhas_cabecalho++;
        xSemaphoreGive(mutex_dados);
        return PACOTE_CABECALHO_INVALIDO;
    }
    if (*crc_calculado != pacote->crc16) {
        dados_compartilhados.pacotes_invalidos++;
        dados_compartilhados.falhas_crc++;
        xSemaphoreGive(mutex_dados);
        return PACOTE_CRC_INVALIDO;
    }
    if (!protocolo_telemetria_conteudo_valido(pacote)) {
        dados_compartilhados.pacotes_invalidos++;
        dados_compartilhados.falhas_conteudo++;
        xSemaphoreGive(mutex_dados);
        return PACOTE_CONTEUDO_INVALIDO;
    }

    bool reinicio = false;
    if (dados_compartilhados.pacotes_validos > 0) {
        if (pacote->tempo_mestre_ms < dados_compartilhados.tempo_mestre_ms) {
            dados_compartilhados.reinicios_mestre++;
            reinicio = true;
            ESP_LOGW(TAG, "Reinicio do mestre detectado; sequencia resincronizada");
        } else {
            uint16_t perdidos = 0;
            const resultado_sequencia_telemetria_t sequencia =
                protocolo_telemetria_classificar_sequencia(
                    true, dados_compartilhados.sequencia, pacote->sequencia, &perdidos);
            if (sequencia == SEQUENCIA_TELEMETRIA_DUPLICADA) {
                dados_compartilhados.pacotes_duplicados++;
            } else if (sequencia == SEQUENCIA_TELEMETRIA_COM_PERDA) {
                dados_compartilhados.pacotes_perdidos += perdidos;
            } else if (sequencia == SEQUENCIA_TELEMETRIA_FORA_DE_ORDEM) {
                dados_compartilhados.pacotes_invalidos++;
                dados_compartilhados.falhas_conteudo++;
                xSemaphoreGive(mutex_dados);
                return PACOTE_CONTEUDO_INVALIDO;
            }
        }
    }

    const uint32_t agora_ms = (uint32_t)(esp_timer_get_time() / 1000);
    atualizar_metricas_comunicacao(pacote, agora_ms, reinicio);
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
    dados_compartilhados.marcha = pacote->marcha & 0x7Fu;
    dados_compartilhados.simulado = (pacote->marcha & 0x80u) != 0;
    dados_compartilhados.velocidade_centesimos_kmh = pacote->velocidade_centesimos_kmh;
    dados_compartilhados.aceleracao_milesimos_ms2 = pacote->aceleracao_milesimos_ms2;
    dados_compartilhados.acelerador_decimos_percentual = pacote->acelerador_decimos_percentual;
    dados_compartilhados.freio_decimos_percentual = pacote->freio_decimos_percentual;
    dados_compartilhados.tensao_pacote_mv = pacote->tensao_pacote_mv;
    dados_compartilhados.corrente_centesimos_a = pacote->corrente_centesimos_a;
    dados_compartilhados.carga_decimos_percentual = pacote->carga_decimos_percentual;
    memcpy(dados_compartilhados.tensoes_celulas_mv, pacote->tensoes_celulas_mv,
           sizeof(dados_compartilhados.tensoes_celulas_mv));
    dados_compartilhados.latitude_micrograus = pacote->latitude_micrograus;
    dados_compartilhados.longitude_micrograus = pacote->longitude_micrograus;
    dados_compartilhados.rumo_decimos_grau = pacote->rumo_decimos_grau;
    memcpy(dados_compartilhados.distancias_cm, pacote->distancias_cm,
           sizeof(dados_compartilhados.distancias_cm));
    dados_compartilhados.potencia_w = pacote->potencia_w;
    dados_compartilhados.autonomia_decimos_km = pacote->autonomia_decimos_km;
    dados_compartilhados.percurso_metros = pacote->percurso_metros;
    dados_compartilhados.odometro_metros = pacote->odometro_metros;
    dados_compartilhados.pacotes_validos++;
    const uint64_t total_esperado = (uint64_t)dados_compartilhados.pacotes_validos +
                                    dados_compartilhados.pacotes_perdidos;
    dados_compartilhados.taxa_perda_centesimos_percentual = total_esperado == 0
        ? 0u : (uint32_t)(((uint64_t)dados_compartilhados.pacotes_perdidos * 10000u) /
                          total_esperado);
    dados_compartilhados.ultimo_recebimento_ms = agora_ms;
    xSemaphoreGive(mutex_dados);
    enviar_confirmacao(pacote);
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
    ESP_LOGI(TAG, "RX v%u seq=%u t=%lu ms %s marcha=%u velocidade=%.2f km/h carga=%.1f%% | ADC=%u",
             TELEMETRIA_VERSAO_PROTOCOLO,
             (unsigned)dados.sequencia, (unsigned long)dados.tempo_mestre_ms,
             dados.simulado ? "SIMULADA" : "REAL", (unsigned)dados.marcha,
             dados.velocidade_centesimos_kmh / 100.0,
             dados.carga_decimos_percentual / 10.0,
             (unsigned)dados.tensao_adc_bruta);
    ESP_LOGI(TAG,
             "PROTOCOLO v%u ok=%lu invalido=%lu crc=%lu cabecalho=%lu conteudo=%lu perdidos=%lu duplicados=%lu",
             TELEMETRIA_VERSAO_PROTOCOLO,
             (unsigned long)dados.pacotes_validos, (unsigned long)dados.pacotes_invalidos,
             (unsigned long)dados.falhas_crc, (unsigned long)dados.falhas_cabecalho,
             (unsigned long)dados.falhas_conteudo,
             (unsigned long)dados.pacotes_perdidos, (unsigned long)dados.pacotes_duplicados);
    ESP_LOGI(TAG,
             "LINK taxa=%.2f pkt/s intervalo=%lu ms jitter=%lu ms atraso_relativo=%lu ms perda=%.2f%%",
             dados.taxa_pacotes_centesimos_hz / 100.0,
             (unsigned long)dados.intervalo_medio_ms,
             (unsigned long)dados.jitter_medio_ms,
             (unsigned long)dados.latencia_relativa_ms,
             dados.taxa_perda_centesimos_percentual / 100.0);
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

static esp_err_t enviar_quadro_enlace(const void *dados, size_t tamanho)
{
    return gerenciador_uart_enviar(dados, tamanho);
}

static void processar_quadro_telemetria(const uint8_t *quadro)
{
    pacote_telemetria_t pacote;
    uint16_t crc_calculado;
    memcpy(&pacote, quadro, sizeof(pacote));
    resultado_validacao_t resultado = validar_e_salvar_pacote(&pacote, &crc_calculado);
    if (resultado == PACOTE_CORRETO) {
        registrar_estado();
    } else {
        registrar_pacote_invalido(quadro, resultado, crc_calculado);
    }
}

static void tarefa_recepcao_uart(void *argumento)
{
    (void)argumento;
    uint8_t lote[256];
    parser_enlace_uart_t parser;
    protocolo_enlace_uart_inicializar(&parser, TELEMETRIA_INICIO_2,
                                      TAMANHO_PACOTE_TELEMETRIA);

    while (true) {
        int recebidos = gerenciador_uart_receber(lote, sizeof(lote), pdMS_TO_TICKS(100));
        if (recebidos <= 0) {
            servico_ota_satelite_verificar_timeout();
            continue;
        }
        for (int i = 0; i < recebidos; i++) {
            const resultado_parser_enlace_uart_t resultado =
                protocolo_enlace_uart_processar_byte(&parser, lote[i]);
            if (resultado == ENLACE_UART_TELEMETRIA_COMPLETA) {
                processar_quadro_telemetria(parser.quadro);
            } else if (resultado == ENLACE_UART_OTA_COMPLETA) {
                const esp_err_t erro = servico_ota_satelite_processar_quadro(
                    parser.quadro, parser.ultimo_tamanho);
                if (erro != ESP_OK) {
                    ESP_LOGW(TAG, "Quadro OTA rejeitado: %s", esp_err_to_name(erro));
                }
            } else if (resultado == ENLACE_UART_COMANDO_COMPLETO) {
                servico_comandos_satelite_processar_quadro(
                    parser.quadro, parser.ultimo_tamanho);
            }
        }
        if (recebidos == (int)sizeof(lote)) vTaskDelay(pdMS_TO_TICKS(1));
    }
}

esp_err_t receptor_uart_iniciar(void)
{
    if (mutex_dados == NULL) mutex_dados = xSemaphoreCreateMutex();
    if (mutex_dados == NULL) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(gerenciador_uart_iniciar(), TAG, "gerenciador UART");
    const configuracao_servico_ota_satelite_t configuracao_ota = {
        .nome_satelite = "equipe",
        .enviar = enviar_quadro_enlace,
        .tempo_limite_sem_dados_ms = 15000,
    };
    ESP_RETURN_ON_ERROR(servico_ota_satelite_iniciar(&configuracao_ota), TAG,
                        "servico OTA do satelite");
    const configuracao_servico_comandos_satelite_t configuracao_comandos = {
        .nome_satelite = "equipe",
        .no_satelite = NO_COMANDO_EQUIPE,
        .capacidades_adicionais = 0,
        .enviar = enviar_quadro_enlace,
    };
    ESP_RETURN_ON_ERROR(
        servico_comandos_satelite_iniciar(&configuracao_comandos), TAG,
        "servico de comandos do satelite");
    if (xTaskCreate(tarefa_recepcao_uart, "recepcao_uart", 8192, NULL, 5, NULL) != pdPASS) {
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
