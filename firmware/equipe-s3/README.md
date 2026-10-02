# Firmware do ESP32-S3 da equipe

Recebe a telemetria do mestre por UART, valida os quadros e oferece um ponto de
acesso Wi-Fi com painel web para a equipe.

O painel em `http://192.168.4.1` também mostra taxa de pacotes, intervalo,
jitter, atraso relativo, perdas e idade da última amostra.
Sua página principal prioriza os indicadores de condução e energia, organiza os
sensores por contexto e mantém a qualidade do enlace em uma área técnica
separada. O layout se adapta a computadores, tablets e celulares sem depender
de bibliotecas ou recursos externos.
A página `http://192.168.4.1/diagnostico` apresenta memória, tempo ativo,
clientes Wi-Fi, estatísticas do protocolo e erros físicos da UART.

A página `http://192.168.4.1/mestre` oferece o controle remoto do mestre. Ela
separa consultas de ações, pede confirmação antes de executar uma ação e mostra
o identificador da solicitação, o número de tentativas e o tempo até a resposta
confirmada pela UART. A visão geral reúne os estados principais; ao selecionar
uma consulta, a página passa a mostrar somente os dados daquele assunto. O
painel acompanha o estado OTA de cada firmware e mantém um histórico local dos
comandos enviados pelo navegador.

## Rádio Wi-Fi

`include/configuracao.h` concentra as opções do Access Point:

- `WIFI_CANAL`: canal de 1 a 13;
- `WIFI_LARGURA_CANAL_MHZ`: `20` ou `40` MHz;
- `WIFI_POTENCIA_MAXIMA_DBM`: potência solicitada entre 2 e 20 dBm.

As opções também podem ser sobrescritas por flags de compilação. O terminal
registra a largura e a potência efetivamente aceitas pelo driver.
