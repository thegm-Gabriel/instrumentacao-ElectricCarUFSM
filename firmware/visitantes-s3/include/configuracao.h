#pragma once

#include "driver/gpio.h"
#include "driver/uart.h"
#include "configuracao_enlace_uart.h"

#define WIFI_SSID "UFSM-Carro-Visitantes"
#define WIFI_SENHA "telemetria2026"
#ifndef WIFI_CANAL
#define WIFI_CANAL 1
#endif
#ifndef WIFI_LARGURA_CANAL_MHZ
#define WIFI_LARGURA_CANAL_MHZ 20
#endif
#ifndef WIFI_POTENCIA_MAXIMA_DBM
#define WIFI_POTENCIA_MAXIMA_DBM 16
#endif
#define WIFI_MAXIMO_CLIENTES 10

#if WIFI_CANAL < 1 || WIFI_CANAL > 13
#error "WIFI_CANAL deve estar entre 1 e 13"
#endif
#if WIFI_LARGURA_CANAL_MHZ != 20 && WIFI_LARGURA_CANAL_MHZ != 40
#error "WIFI_LARGURA_CANAL_MHZ deve ser 20 ou 40"
#endif
#if WIFI_POTENCIA_MAXIMA_DBM < 2 || WIFI_POTENCIA_MAXIMA_DBM > 20
#error "WIFI_POTENCIA_MAXIMA_DBM deve estar entre 2 e 20 dBm"
#endif

#define UART_TELEMETRIA UART_NUM_1
#define UART_BAUD_RATE ENLACE_UART_BAUD_RATE
#define PINO_UART_TX GPIO_NUM_6
#define PINO_UART_RX GPIO_NUM_5
#define UART_TAMANHO_BUFFER 8192

#define IP_PAINEL "192.168.4.1"
#define PORTA_HTTP 80
#define PORTA_DNS 53
