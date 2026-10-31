// Centraliza pinagem, rede e limites do satélite da equipe.
#pragma once

#include "driver/gpio.h"
#include "driver/uart.h"
#include "configuracao_enlace_uart.h"

#define WIFI_SSID "UFSM-Carro-Equipe"
#define WIFI_SENHA "telemetria2026"
#define WIFI_IP_AP_TEXTO "192.168.4.1"
#define URL_IP_LOCAL_BASE "http://" WIFI_IP_AP_TEXTO
#define URL_IP_LOCAL_EQUIPE URL_IP_LOCAL_BASE "/painel"
#define URL_IP_PORTAL_ENTRADA URL_IP_LOCAL_BASE "/"
#ifndef WIFI_CANAL
#define WIFI_CANAL 11
#endif
#define WIFI_MAXIMO_CLIENTES 5

#if WIFI_CANAL < 1 || WIFI_CANAL > 13
#error "WIFI_CANAL deve estar entre 1 e 13"
#endif
#define UART_TELEMETRIA UART_NUM_1
#define UART_BAUD_RATE ENLACE_UART_BAUD_RATE
#define PINO_UART_TX GPIO_NUM_6
#define PINO_UART_RX GPIO_NUM_5
#define UART_TAMANHO_BUFFER_RX 8192
#define UART_TAMANHO_BUFFER_TX 2048
#define UART_TEMPO_LIMITE_TX_MS 20

#define PORTA_HTTP 80
