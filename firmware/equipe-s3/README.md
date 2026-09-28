# Firmware do ESP32-S3 da equipe

Recebe a telemetria do mestre por UART, valida os quadros e oferece um ponto de
acesso Wi-Fi com painel web para a equipe.

O painel em `http://192.168.4.1` também mostra taxa de pacotes, intervalo,
jitter, atraso relativo, perdas e idade da última amostra.
A página `http://192.168.4.1/diagnostico` apresenta memória, tempo ativo,
clientes Wi-Fi, estatísticas do protocolo e erros físicos da UART.
