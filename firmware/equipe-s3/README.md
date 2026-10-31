# Firmware do ESP32-S3 da equipe

Recebe a telemetria do mestre por UART, valida os quadros e oferece um ponto de
acesso Wi-Fi com painel web para a equipe.

O [guia técnico dos satélites](../../documentacao/satelites/README.md) detalha
o enlace bidirecional, as rotas HTTP, os campos da API e as ações disponíveis.

Conecte-se à rede `UFSM-Carro-Equipe` e abra `http://192.168.4.1/` ou
`http://192.168.4.1/painel`. O monitor serial registra a partida do AP, o IP
entregue por DHCP, o início do HTTP e as requisições à página principal.

## Organização

`src/main.c` apenas inicializa os módulos. `include/nucleo` reúne configuração e
tipos de telemetria; `gerenciadores` contém os acessos a Wi-Fi e UART;
`servicos` reúne recepção, diagnóstico e painel web. O código fonte da interface
fica em `web/index.html`, `web/style.css`, `web/layout.css` e `web/app.js`. Execute
`python web/gerar_painel.py` para gerar `src/servicos/painel_web.c`, que embute a
página no firmware sem dependências externas. OTA e comandos permanecem em
`firmware/compartilhado`, usados pelos dois satélites.

Todas as rotas de página (`/`, `/painel`, `/diagnostico` e `/mestre`) abrem a
interface NEXUS, com exatamente três abas: **ESP32 — Mestre**, **ESP32 — Equipe**
e **ESP32 — Visitantes**. A telemetria do carro fica com o mestre. Os dados vêm
de `/api/telemetria`, `/api/diagnostico` e `/api/mestre`. As três abas são
atualizadas automaticamente, inclusive diagnóstico e versão do firmware. As
consultas remotas passam por uma fila para não disputar o enlace UART.

A aba **ESP32 — Equipe** também envia comandos ao mestre: consultas manuais,
controle de OTA, relatórios, percurso e configuração de Wi-Fi. Os controles
aparecem conforme as capacidades anunciadas pelo firmware. A página confirma
cada ação antes do envio e registra a resposta recebida; ações assíncronas
devem ser acompanhadas pelas consultas de estado.

Os gráficos e o CSV usam somente amostras recebidas enquanto a página está
aberta. A API atual não oferece sessões anteriores, gravação remota, RSSI dos
enlaces UART ou confirmação do efeito final de ações assíncronas. A interface
marca esses itens como indisponíveis e diferencia uma solicitação aceita da
execução confirmada. No modo real atual, tensão, corrente, carga e pedais ainda
não são adquiridos; os valores correspondentes só aparecem quando o firmware
identifica a telemetria como simulada. O painel não usa bibliotecas, fontes ou
recursos da internet.

## Rádio Wi-Fi

`include/nucleo/configuracao.h` concentra SSID, senha, canal e número máximo de
clientes. A interface padrão SoftAP do ESP-IDF usa `192.168.4.1` e fornece os
endereços dos clientes por DHCP. O firmware não cria servidor DNS nem altera
manualmente a largura de canal e a potência do rádio. O DHCP anuncia
`http://192.168.4.1/` pela opção 114 para sugerir a abertura do painel ao
celular; o resultado depende do sistema e do navegador. Se o celular fizer uma
sonda HTTP ao ESP, caminhos desconhecidos são redirecionados ao painel. Sem DNS
local, nomes como `painel.equipe` não são oferecidos; use o endereço por IP.
