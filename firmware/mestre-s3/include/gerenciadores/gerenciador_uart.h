// Declara filas, recepção e recuperação dos dois canais UART.
#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "nucleo/destino_enlace.h"

struct EstatisticasUart {
    bool inicializado = false;
    uint32_t envios_ok = 0;
    uint32_t erros_envio = 0;
    uint32_t mensagens_enfileiradas = 0;
    uint32_t mensagens_descartadas = 0;
    uint16_t ocupacao_fila_tx = 0;
    uint16_t maior_ocupacao_fila_tx = 0;
    uint32_t recepcoes_ok = 0;
    uint32_t erros_recepcao = 0;
    uint64_t bytes_enviados = 0;
    uint64_t bytes_recebidos = 0;
    uint32_t estouros_fifo = 0;
    uint32_t buffers_cheios = 0;
    uint32_t erros_quadro = 0;
    uint32_t erros_paridade = 0;
    uint32_t sinais_break = 0;
    uint32_t recuperacoes = 0;
    esp_err_t ultimo_erro_envio = ESP_OK;
    esp_err_t ultimo_erro_recepcao = ESP_OK;
};

esp_err_t gerenciador_uart_iniciar();
esp_err_t gerenciador_uart_encerrar();
// Copia um quadro binário para a fila de transmissão. ESP_OK confirma apenas
// o enfileiramento; a confirmação fim a fim pertence ao protocolo chamador.
esp_err_t gerenciador_uart_enfileirar(
    DestinoUart destino, const void* dados, size_t tamanho,
    TickType_t tempo_limite = pdMS_TO_TICKS(20));
// Envia e aguarda a saída física do quadro. É usado por transferências
// confirmadas para evitar cópia e latência adicionais na fila assíncrona.
esp_err_t gerenciador_uart_enviar(
    DestinoUart destino, const void* dados, size_t tamanho,
    TickType_t tempo_limite = pdMS_TO_TICKS(250));
int gerenciador_uart_receber(DestinoUart origem, void* buffer, size_t capacidade,
                             TickType_t tempo_limite);
esp_err_t gerenciador_uart_recuperar(DestinoUart destino);
EstatisticasUart gerenciador_uart_obter_estatisticas(DestinoUart destino);
