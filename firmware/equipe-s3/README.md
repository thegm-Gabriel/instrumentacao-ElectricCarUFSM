# Firmware do ESP32-S3 da equipe

Recebe a telemetria do mestre por UART, valida os quadros e oferece um ponto de
acesso Wi-Fi com painel web para a equipe.

O painel em `http://painel.equipe` também mostra taxa de pacotes, intervalo,
jitter, atraso relativo, perdas e idade da última amostra.
Sua página principal prioriza os indicadores de condução e energia, organiza os
sensores por contexto e mantém a qualidade do enlace em uma área técnica
separada. O layout se adapta a computadores, tablets e celulares sem depender
de bibliotecas ou recursos externos.
A página `http://painel.diagnostico` apresenta memória e fragmentação,
causa da inicialização, rádio, clientes Wi-Fi, qualidade da telemetria,
estatísticas completas da UART, comandos e recepção OTA local.

A página `http://painel.mestre` oferece o controle remoto do mestre. Ela
separa consultas de ações, pede confirmação antes de executar uma ação e mostra
o identificador da solicitação, o número de tentativas e o tempo até a resposta
confirmada pela UART. A visão geral reúne os estados principais; ao selecionar
uma consulta, a página passa a mostrar somente os dados daquele assunto. O
painel acompanha o estado OTA de cada firmware, mantém um histórico local e
oferece as operações do terminal serial para relatórios, percurso, registro no
Google, intervalos e gerenciamento de redes Wi-Fi.

As três rotas permanecem separadas de propósito, mas compartilham a mesma
identidade visual e navegação. A página principal é dedicada à telemetria de
alta frequência, o diagnóstico concentra contadores técnicos e a página do
mestre isola comandos e ações que alteram estado. Isso reduz o conteúdo e o
JavaScript carregados em cada acesso e evita misturar ações críticas com a tela
usada continuamente durante a operação do veículo.

## Rádio Wi-Fi

`include/configuracao.h` concentra as opções do Access Point:

- `WIFI_CANAL`: canal de 1 a 13;
- `WIFI_LARGURA_CANAL_MHZ`: `20` ou `40` MHz;
- `WIFI_POTENCIA_MAXIMA_DBM`: potência solicitada entre 2 e 20 dBm.

As opções também podem ser sobrescritas por flags de compilação. O terminal
registra a largura e a potência efetivamente aceitas pelo driver.

O DHCP anuncia o próprio ESP como servidor DNS e informa a URL do portal
cativo. O DNS local direciona `painel.equipe`, `painel.diagnostico` e
`painel.mestre` ao mesmo servidor enquanto o dispositivo estiver conectado à
rede `UFSM-Carro-Equipe`. O cabeçalho HTTP `Host` seleciona a página correta;
endereços desconhecidos são redirecionados para `painel.equipe`.

Os nomes pertencem somente à rede local criada pelo ESP e devem ser abertos com
`http://`. Eles não são domínios públicos e não funcionam fora desse ponto de
acesso. Navegadores configurados para forçar HTTPS ou DNS seguro podem impedir a
resolução; nesse caso, libere HTTP para a rede local e tente novamente.
