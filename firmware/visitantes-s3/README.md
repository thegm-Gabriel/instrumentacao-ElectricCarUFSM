# Firmware do ESP32-S3 de visitantes

Recebe a telemetria do mestre por UART, valida os quadros e oferece um ponto de
acesso Wi-Fi com painel web para visitantes.

O [guia técnico dos satélites](../../documentacao/satelites/README.md) detalha
o enlace bidirecional, a rede local, o DNS cativo e os campos da API pública.

## Organização

`src/main.c` apenas inicializa os módulos. `include/nucleo` reúne configuração e
tipos de telemetria; `gerenciadores` contém o acesso ao Wi-Fi; `servicos`
reúne recepção UART e painel web. OTA e comandos permanecem em
`firmware/compartilhado`, usados pelos dois satélites.

A página pública fica em `web/index.html`, `web/style.css` e `web/app.js`.
Execute `python web/gerar_painel.py` para gerar
`src/servicos/painel_web.c`, que incorpora a página ao firmware. Ela consulta
somente `/api/telemetria`, sem oferecer diagnósticos, comandos ou configurações.
O endpoint publica apenas os campos necessários ao painel; no modo real atual,
grandezas elétricas ainda não adquiridas são retornadas como `null`.

O histórico dos gráficos e os cálculos de energia, distância e tempo começam
quando a página é aberta. Valores simulados são identificados, e leituras
interrompidas permanecem na tela com indicação de desatualização.

## Rádio Wi-Fi

`include/nucleo/configuracao.h` concentra as opções do Access Point:

- `WIFI_CANAL`: canal de 1 a 13;
- `WIFI_LARGURA_CANAL_MHZ`: `20` ou `40` MHz;
- `WIFI_POTENCIA_MAXIMA_DBM`: potência solicitada entre 2 e 20 dBm.

As opções também podem ser sobrescritas por flags de compilação. O terminal
registra a largura e a potência efetivamente aceitas pelo driver.
