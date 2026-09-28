#pragma once

#include "driver/gpio.h"
#include "driver/uart.h"

#define WIFI_SSID "UFSM-Carro-Equipe"
#define WIFI_SENHA "telemetria2026"
#define WIFI_CANAL 1
#define WIFI_MAXIMO_CLIENTES 4

#define UART_TELEMETRIA UART_NUM_1
#define UART_BAUD_RATE 921600
#define PINO_UART_TX GPIO_NUM_6
#define PINO_UART_RX GPIO_NUM_5
#define UART_TAMANHO_BUFFER_RX 8192
#define UART_TAMANHO_BUFFER_TX 2048
#define UART_TEMPO_LIMITE_TX_MS 20

#define IP_PAINEL "192.168.4.1"
#define PORTA_HTTP 80
#define PORTA_DNS 53
