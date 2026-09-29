# Firmware do ESP32-S3 de visitantes

Recebe a telemetria do mestre por UART, valida os quadros e oferece um ponto de
acesso Wi-Fi com painel web para visitantes.

## Rádio Wi-Fi

`include/configuracao.h` concentra as opções do Access Point:

- `WIFI_CANAL`: canal de 1 a 13;
- `WIFI_LARGURA_CANAL_MHZ`: `20` ou `40` MHz;
- `WIFI_POTENCIA_MAXIMA_DBM`: potência solicitada entre 2 e 20 dBm.

As opções também podem ser sobrescritas por flags de compilação. O terminal
registra a largura e a potência efetivamente aceitas pelo driver.
