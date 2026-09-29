#pragma once

/*
 * Camada física comum aos três firmwares.
 *
 * O ESP32-S3 consegue gerar 1 Mbaud de forma exata usando o clock APB de
 * 80 MHz. Manter a taxa neste cabeçalho evita compilar mestre e satélites com
 * valores incompatíveis.
 */
#define ENLACE_UART_BAUD_RATE 1000000

