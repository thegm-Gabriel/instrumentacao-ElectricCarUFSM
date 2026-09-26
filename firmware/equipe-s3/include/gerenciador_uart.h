#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef struct {
    bool inicializado;
    uint32_t envios_ok;
    uint32_t erros_envio;
    uint32_t recepcoes_ok;
    uint32_t erros_recepcao;
    uint64_t bytes_enviados;
    uint64_t bytes_recebidos;
    uint32_t estouros_fifo;
    uint32_t buffers_cheios;
    uint32_t erros_quadro;
    uint32_t erros_paridade;
    uint32_t sinais_break;
    esp_err_t ultimo_erro_envio;
    esp_err_t ultimo_erro_recepcao;
} estatisticas_gerenciador_uart_t;

// Inicializa a UART do módulo. Pode ser chamada novamente sem duplicar o driver.
esp_err_t gerenciador_uart_iniciar(void);

// Funções genéricas para protocolos atuais e futuros.
esp_err_t gerenciador_uart_enviar(const void *dados, size_t tamanho);
esp_err_t gerenciador_uart_enviar_com_timeout(const void *dados, size_t tamanho,
                                              TickType_t tempo_limite);
int gerenciador_uart_receber(void *buffer, size_t capacidade, TickType_t tempo_limite);
esp_err_t gerenciador_uart_limpar_recepcao(void);
void gerenciador_uart_obter_estatisticas(estatisticas_gerenciador_uart_t *estatisticas);
