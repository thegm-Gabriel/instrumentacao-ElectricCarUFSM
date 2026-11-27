// Gerencia a UART do enlace do satélite de visitantes.
#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

esp_err_t gerenciador_uart_iniciar(void);
int gerenciador_uart_receber(void *buffer, size_t capacidade, TickType_t tempo_limite);
// Escreve o quadro e retorna a quantidade de bytes aceitos pelo driver.
int gerenciador_uart_escrever(const void *dados, size_t tamanho);
// Escreve o quadro e aguarda a conclusão da transmissão.
esp_err_t gerenciador_uart_enviar(const void *dados, size_t tamanho);
