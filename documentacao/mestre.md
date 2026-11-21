# Guia técnico do firmware mestre

[Índice da documentação](README.md) · [Conceitos e periféricos](conceitos.md)

Este guia descreve o comportamento implementado em `firmware/mestre-s3` no estado atual do código. A [arquitetura](arquitetura.md) explica a separação entre módulos e a [pinagem geral](pinagem.md) detalha as ligações elétricas. Os valores abaixo são **definições de firmware**; a montagem física e os níveis elétricos precisam ser conferidos na bancada.

## Escopo e fluxo de dados

O alvo é o ESP32-S3-WROOM-1-N16R8, com 16 MiB de flash e 8 MiB de PSRAM Octal. O mestre lê a contagem bruta do ADC, tensão e corrente pelo INA228, movimento pelo MPU6050 e deslocamento de dois encoders. A tarefa `telemetria` reúne as leituras em `DadosTelemetriaVeiculo`, publica a amostra local consultada pelos relatórios, envia os quadros aos dois satélites e entrega amostras ao registro em SD. A fonte padrão fica em [`veiculo.h`](../firmware/mestre-s3/include/nucleo/configuracao/veiculo.h) e pode ser trocada temporariamente pelo painel da equipe. Consulte [a composição em `main.cpp`](../firmware/mestre-s3/src/main.cpp), [o modelo compartilhado](../firmware/compartilhado/telemetria/dados_telemetria_veiculo.h) e [a configuração da placa](../firmware/mestre-s3/include/nucleo/configuracao/placa.h).

Cada módulo tem sua implementação em `src/` e seu cabeçalho em `include/`, com a mesma estrutura de pastas.

| Local | Responsabilidade |
|---|---|
| [`main.cpp`](../firmware/mestre-s3/src/main.cpp) | Composição, ordem de partida, tolerância a falhas e confirmação ou rejeição de OTA no boot. |
| [`include/nucleo`](../firmware/mestre-s3/include/nucleo) | Cabeçalhos de base, incluindo tipos compartilhados como o destino UART. |
| [`include/nucleo/configuracao`](../firmware/mestre-s3/include/nucleo/configuracao) | Valores padrão do mestre, separados por assunto. |
| [`gerenciadores`](../firmware/mestre-s3/src/gerenciadores) | Acesso a UART, I²C, PCNT, SD, Wi-Fi, indicadores e OTA local via ESP-IDF. |
| [`dispositivos/entradas`](../firmware/mestre-s3/src/dispositivos/entradas) | Leitor ADC e pedal, chave de marcha, encoders, INA228 e MPU6050. |
| [`dispositivos/saidas`](../firmware/mestre-s3/src/dispositivos/saidas) | DAC MCP4725, saídas digitais do veículo e tela SSD1306. |
| [`servicos/veiculo`](../firmware/mestre-s3/src/servicos/veiculo) | Ciclo de aquisição e pausa de sensores, aceleração, odômetro, simulador e controle do veículo. |
| [`servicos/comunicacao`](../firmware/mestre-s3/src/servicos/comunicacao) | Enlace, telemetria e comandos UART, sessões Wi-Fi e relógio HTTP. |
| [`servicos/historico`](../firmware/mestre-s3/src/servicos/historico) | Registro e backup no SD e envio de CSV ao Google. |
| [`servicos/manutencao`](../firmware/mestre-s3/src/servicos/manutencao) | OTA do mestre e dos satélites, diagnóstico e supervisão. |
| [`servicos/interface_local`](../firmware/mestre-s3/src/servicos/interface_local) | Terminal, OLED, sinalização e relatórios. |

Consulte a [arquitetura](arquitetura.md) para os contratos entre os módulos e os fluxos de recuperação.

## Pinout do mestre

Fonte canônica dos GPIOs: [`placa.h`](../firmware/mestre-s3/include/nucleo/configuracao/placa.h). O barramento UART usa 3,3 V, 8N1 e 1.000.000 baud conforme [`configuracao_enlace_uart.h`](../firmware/compartilhado/protocolos/configuracao_enlace_uart.h). Mestre e satélites compartilham GND.

| Função | Interface | GPIO mestre | Observação |
|---|---|---|---|
| Leitura do pedal Hall | ADC1 | 3 | A leitura do pedal é calibrada e usada pelo serviço de controle. |
| Leitura de tensão | ADC | 4 | A entrada deve respeitar o limite do ADC; o condicionamento externo é necessário para tensões maiores. |
| Encoder esquerdo A/B | PCNT | 5 / 6 | Saídas NPN exigem pull-ups externos para **3,3 V**. |
| Encoder direito A/B | PCNT | 7 / 21 | Mesma exigência de pull-ups. |
| MPU6050, INA228 e SSD1306 SDA/SCL | I²C0 | 8 / 9 | 400 kHz; MPU em `0x68`, INA em `0x40` e tela em `0x3C` ou `0x3D`. |
| LEDs verde/amarelo/vermelho/azul | GPIO | 10 / 11 / 12 / 13 | Ativos em nível alto, cada LED com resistor em série. |
| Buzzer passivo | PWM/LEDC | 14 | Acionamento externo recomendado na pinagem geral. |
| UART2 visitantes TX/RX | UART | 15 / 16 | Cruzar TX com RX do satélite. |
| UART1 equipe TX/RX | UART | 17 / 18 | Cruzar TX com RX do satélite. |
| microSD CLK/MOSI/MISO/CS | SPI2 | 38 / 39 / 40 / 41 | Módulo alimentado em 3,3 V; frequência configurada de 10 MHz. |

Os GPIOs 33 a 37 ficam reservados para a PSRAM Octal do módulo N16R8. O SD e o barramento de encoders devem ser validados no hardware montado; a tabela registra a configuração de software. A [pinagem geral](pinagem.md) inclui recomendações de alimentação e ligação.

## Partida, degradação e recuperação

[`app_main`](../firmware/mestre-s3/src/main.cpp) verifica primeiro a flash de 16 MiB. Flash incompatível interrompe a partida; PSRAM ausente ou com tamanho inesperado gera aviso e os recursos opcionais tentam RAM interna. Inicializa sinalização e tela, depois NVS, supervisão, enlaces, preparação da aquisição, controle do veículo, registro SD, tarefa de telemetria, Wi-Fi, OTA, terminal, relatórios e diagnóstico. A NVS não é apagada automaticamente quando precisa de manutenção, pois contém odômetro e perfis Wi-Fi.

Falhas de NVS, inicialização do núcleo da aplicação, criação/supervisão da tarefa de telemetria ou confirmação da imagem OTA pendente provocam rejeição da imagem e reinício. Falhas isoladas de ADC, INA228, MPU6050, odômetro, SD, tela, LEDs, Wi-Fi, terminal, relatórios e diagnóstico permitem operação degradada com aviso. Sem cartão, a telemetria ao vivo continua e o registro é tentado novamente a cada 30 s. Uma imagem OTA pendente permanece em observação por 2 min depois de os serviços iniciarem; reinício antes da confirmação aciona o rollback do bootloader. Até 5 min após o primeiro boot, três reinícios por panic ou watchdog também solicitam rollback. A contagem usa memória RTC e se perde após corte total de energia. Fonte: [`main.cpp`](../firmware/mestre-s3/src/main.cpp) e [`guarda_rollback_ota.h`](../firmware/compartilhado/ota/guarda_rollback_ota.h).

## Aquisição e telemetria

`USAR_TELEMETRIA_SIMULADA=false` seleciona os sensores reais na partida; `true` usa [`simulador.cpp`](../firmware/mestre-s3/src/servicos/veiculo/simulador.cpp). O painel da equipe permite ativar ou desativar a simulação durante a sessão; ao reiniciar, retorna ao valor definido no código. A consulta remota informa fonte atual, padrão, dinâmica e amostras geradas. Na simulação, o estado de carga começa em 88% e diminui 0,1 ponto percentual a cada 250 ms de tempo real com a simulação ativa; chega a zero após cerca de 3 min 40 s. Esse ciclo acelerado serve para testar a interface e não representa a capacidade nem o consumo reais da bateria. Ao zerar, o movimento e a corrente simulados param; desativar e reativar a simulação reinicia a carga demonstrativa em 88%. O ciclo normal é de 250 ms (4 Hz). ADC indisponível, leitura INA228 ou MPU inválida e falha no odômetro são contabilizados ou sinalizados; uma falha pontual não derruba o restante do ciclo. O MPU é amostrado por serviço próprio a 100 Hz, com calibração, compensação de gravidade, filtro e histerese para o sentido longitudinal. A montagem codificada assume X vertical e Y longitudinal positivo para frente. Fontes: [`aquisicao.cpp`](../firmware/mestre-s3/src/servicos/veiculo/aquisicao.cpp), [`aceleracao.cpp`](../firmware/mestre-s3/src/servicos/veiculo/aceleracao.cpp).

### Monitor elétrico INA228

[`ina228.cpp`](../firmware/mestre-s3/src/dispositivos/entradas/ina228.cpp) usa o mesmo I²C do MPU6050 e da OLED, no endereço `0x40` (A0 e A1 em GND). Confere os identificadores da TI e do dispositivo, configura o shunt de **1 mΩ** e a corrente máxima esperada de **75 A**, grava `SHUNT_CAL` e verifica a leitura de volta. A faixa diferencial é de ±163,84 mV. O ADC interno mede continuamente barramento, shunt e temperatura, com conversões de 540 µs e média de 16; a tarefa de telemetria consulta os resultados a 4 Hz. O sentido positivo da corrente significa consumo; se a instalação do shunt inverter esse sinal, ajuste `SINAL_CORRENTE_INA228` na [configuração da placa](../firmware/mestre-s3/include/nucleo/configuracao/placa.h).

O driver disponibiliza tensão de barramento, queda no shunt, corrente, temperatura do chip, potência, energia acumulada e carga acumulada. A potência enviada aos painéis é `VBUS × corrente`, com sinal, porque o registrador `POWER` do INA228 é **sem sinal**; corrente e potência negativas indicam regeneração. O acumulador de energia do chip também é sem sinal, enquanto o de carga é assinado. Ambos começam no reset do INA228, não persistem em desligamentos e deixam de ser considerados válidos após sinalização de transbordamento. A tela serial **Sensores → INA228** mostra também a idade da leitura e a potência sem sinal do registrador; a página de telemetria da equipe mostra queda no shunt, temperatura do chip, acumuladores e diagnóstico. Essas grandezas não são estimativas da carga restante da bateria nem da autonomia.

Uma leitura elétrica só é publicada quando a identificação, calibração, conversão pronta e dados são válidos. O bit `TELEMETRIA_ELETRICA_VALIDA` do protocolo distingue **zero medido** de valor indisponível; as páginas da equipe e de visitantes mostram tensão, corrente e potência reais apenas com esse bit. Em falha, o mestre limpa as três grandezas daquele ciclo, conta o erro no diagnóstico e, após três erros seguidos, tenta reinicializar o INA228 a cada 5 s. A telemetria de movimento continua. O protocolo transporta tensão até 65,535 V; leituras acima disso são rejeitadas para evitar saturação silenciosa. A instalação deve confirmar que `VBUS` está ligado ao ponto cuja tensão se deseja mostrar, que o shunt tem de fato 1 mΩ e que a corrente máxima esperada corresponde ao circuito.

O simulador sorteia uma semente por inicialização. Cada episódio de 45 s tem
seis trechos de duração e velocidade alvo variáveis: pode acelerar ou desacelerar
devagar ou rapidamente, manter quase a mesma velocidade, parar e, ocasionalmente,
usar marcha à ré. O primeiro episódio termina com uma frenagem curta para
facilitar a conferência do marcador no painel. A interpolação mantém velocidade
e aceleração contínuas nas transições. Acelerador acompanha aceleração positiva;
freio acompanha desaceleração mais forte; corrente negativa representa regeneração
somente enquanto há movimento. Tensão e potência são calculadas da mesma corrente.
A prévia local dos painéis usa o mesmo perfil. A semente é registrada uma vez no
log para reproduzir a sequência.
A dinâmica configurada altera a rapidez do trajeto; nenhuma leitura simulada
atua no veículo.

Os dois encoders de 600 P/R têm as quatro bordas de A e B contadas pelo PCNT
(2.400 contagens/giro). A configuração atual não distingue o sentido de
rotação: avanço e recuo aumentam o percurso. [`odometro.cpp`](../firmware/mestre-s3/src/servicos/veiculo/odometro.cpp)
aplica diâmetro de 0,300 m e relação encoder/roda de 1,0 em cada lado, usa a
média das distâncias para velocidade e distância, rejeita saltos acima do
limite configurado de 200 km/h e acompanha divergência persistente entre
rodas. `percurso_km` recomeça no boot; `odometro_km` é recuperado da NVS e
persistido a cada 100 m. Ajuste esses valores à mecânica real em
[`veiculo.h`](../firmware/mestre-s3/include/nucleo/configuracao/veiculo.h).

[`telemetria_uart.cpp`](../firmware/mestre-s3/src/servicos/comunicacao/telemetria_uart.cpp) envia o mesmo retrato a equipe e visitantes, com fila de transmissão por UART. O [protocolo v5](../firmware/compartilhado/protocolos/protocolo_telemetria.h) define pacote de 122 bytes, carga de 115 bytes, CRC-16/CCITT-FALSE e confirmação de 20 bytes. Os retornos permitem medir RTT e idade da última resposta de cada satélite. [`enlace_uart.cpp`](../firmware/mestre-s3/src/servicos/comunicacao/enlace_uart.cpp) é o consumidor dos quadros de retorno e os encaminha para telemetria, comandos ou OTA. O [parser do enlace](../firmware/compartilhado/protocolos/protocolo_enlace_uart.h) distingue as três famílias de quadro.

Durante OTA de satélite, o intervalo da telemetria passa, por padrão, a 2 s e pode ser alterado entre 500 ms e 10 s; a mudança mantém um canal de observação enquanto a UART transporta o firmware. A cadência padrão fica em [`comunicacao.h`](../firmware/mestre-s3/include/nucleo/configuracao/comunicacao.h). Veja a implementação em [`telemetria_uart.cpp`](../firmware/mestre-s3/src/servicos/comunicacao/telemetria_uart.cpp).

## Armazenamento e envio ao Google

[`registro_sd.cpp`](../firmware/mestre-s3/src/servicos/historico/registro_sd.cpp) reduz o retrato ao vivo para 1 amostra/s e grava CSV em `/sdcard/telemetria`. O arquivo ativo é sincronizado a cada 10 s e gira após 1 h ou 8 MiB, mantendo a mesma quantidade de amostras com menos arquivos. Uma fila de 24 amostras desacopla aquisição e escrita. O envio pendente é solicitado pelo usuário no terminal ou painel da equipe; abre sessão Wi-Fi, transmite o CSV em blocos, tolera escritas HTTPS parciais e só move o arquivo para `/sdcard/telemetria/enviados` após confirmação nominal do Google. Falhas preservam o arquivo para nova tentativa e registram o resultado HTTP quando disponível.

O serviço mantém `/sdcard/logs/esp_00000001.log`, `esp_00000002.log` e assim por diante, sem reiniciar a numeração a cada sessão. Cada arquivo comporta cerca de 8 MiB, com limite de 16 MiB de log por sessão. Em operação normal, grava um resumo relevante a cada 30 s. Uma tarefa independente conserva na PSRAM as últimas 120 amostras de saúde, memória, erros, tarefas críticas e estado/latência dos satélites, a 1 Hz. Sem PSRAM, usa um histórico reduzido de 30 amostras na RAM interna. Na falha detectada, grava uma vez o histórico anterior e o contexto atual (`HIST_INICIO`, `HIST`, `HIST_FIM`), com identificação do incidente; depois usa intervalo de 2 s por 2 min ou, em falha grave, de 1 s por 5 min. Novos erros podem antecipar um registro, respeitando o limite de um por segundo. Uma nova falha grave pode antecipar outra captura. A limitação de capturas evita repetir o mesmo histórico continuamente. Se o SD estiver indisponível, mantém o primeiro lote em RAM para nova tentativa e conta incidentes adicionais que não couberem; esse histórico volátil se perde se o ESP desligar antes da gravação. `TEMPO` informa o tempo desde o boot e UTC quando um cabeçalho `Date` válido é recebido por HTTPS na consulta OTA ou no envio ao Google. Antes disso, a ordem dos arquivos identifica qual é mais recente. O log do SD é um diagnóstico estruturado; as linhas individuais `ESP_LOG*` continuam no console serial. Os logs não são enviados ao Google.

### Backup de configuração

`/sdcard/configuracao_atual.txt` guarda sequência, checksum, odômetro, nomes de
redes, política e preferência de conexão automática do Wi-Fi, intervalo de
verificação OTA, intervalos e pausa dos relatórios e parâmetros fixos do CSV.
O firmware escreve primeiro um arquivo temporário e conclui a substituição a
cada 60 s. Uma falha permite nova tentativa após 5 s; a sequência avança somente
depois de uma gravação concluída.

Na partida, o firmware valida o checksum e escolhe o retrato íntegro mais
recente, incluindo o temporário. A restauração é seletiva:

- O odômetro volta somente se sua chave NVS estiver ausente.
- Política e preferência automática do Wi-Fi voltam somente se não existir a
  configuração de perfis na NVS.
- O intervalo OTA volta somente se sua chave NVS estiver ausente.
- Intervalos e pausa dos relatórios são reaplicados antes de iniciar o serviço;
  esses ajustes não têm persistência própria na NVS.

Os SSIDs são informativos. **O arquivo não contém senhas**, portanto não pode
recriar perfis de rede utilizáveis; depois de limpar a NVS, cadastre as
credenciais novamente. Os parâmetros fixos do CSV pertencem ao firmware e não
são reconfigurados pelo retrato. Um backup inválido não altera a configuração.
Um corte de energia pode deixar até cerca de 1 min de diferença entre o
odômetro real e o último retrato. Consulte [telemetria no Google](google.md)
para as credenciais do Web App, que também não ficam nesse arquivo.


## Wi-Fi, OTA e comandos

O rádio opera como estação sob sessões temporárias. [`wifi_sessoes.cpp`](../firmware/mestre-s3/src/servicos/comunicacao/wifi_sessoes.cpp) tenta primeiro a rede principal, administra até cinco redes secundárias salvas na NVS e exige autorização para tentativas secundárias salvo quando o modo automático foi habilitado. Trocar/restaurar rede principal ou mudar o modo automático requer `WIFI_SENHA_PERMISSAO` (com fallback para `OTA_WIFI_SENHA`). Canal preferencial, largura e potência estão em [`wifi.h`](../firmware/mestre-s3/include/nucleo/configuracao/wifi.h). Senhas privadas pertencem ao arquivo local `include/nucleo/segredos_ota.h`, criado a partir do exemplo e ignorado pelo Git.

No menu serial, a opção 5 permite testar uma rede informada, salvá-la, conectar
uma rede salva ou excluí-la. Para definir a principal, é possível informar uma
rede nova ou selecionar uma já salva; em ambos os casos o mestre testa a
conexão antes de gravar. Uma rede salva promovida deixa a lista de secundárias
e a configuração consultada pela Equipe confirma o SSID principal. A senha
pode ficar vazia para uma rede aberta;
redes protegidas exigem 8 a 63 caracteres. Antes de abrir uma sessão OTA ou
Google, o mestre faz uma varredura única e tenta apenas redes autorizadas que
aparecem nela. Se a varredura falhar, houver mais pontos de acesso do que o
limite da leitura ou apenas redes ocultas não identificáveis, tenta os perfis
na ordem normal; se
nenhuma rede autorizada for vista, registra o motivo e encerra a tentativa.
Redes secundárias exigem confirmação, salvo no modo automático protegido por senha.
As credenciais e preferências ficam na NVS e não aparecem em relatórios ou logs.
`WIFI_CANAL_PREFERENCIAL` vale somente para o início da busca em modo estação:
o roteador determina o canal após a associação, e `0` aceita qualquer canal.
`WIFI_LARGURA_CANAL_MHZ` e `WIFI_POTENCIA_MAXIMA_DBM` definem os outros limites
do rádio. As funções públicas de `wifi_sessoes.h` aplicam a mesma política
quando chamadas por serviços diferentes do terminal.

[`ota.cpp`](../firmware/mestre-s3/src/servicos/manutencao/ota.cpp) verifica por padrão a cada 15 minutos um manifesto de GitHub Release por HTTPS; download exige autorização no terminal ou painel da equipe. Uma oferta sem autorização expira em 5 min e volta ao estado de cancelamento. O manifesto e cada imagem são verificados antes de aplicar. A atualização do mestre usa partições OTA e rollback; nos satélites, [`ota_satelites.cpp`](../firmware/mestre-s3/src/servicos/manutencao/ota_satelites.cpp) faz download HTTPS e envio UART confirmado em pipeline com três blocos reutilizáveis de 4 KiB. O fluxo "todos" atualiza os satélites antes do mestre. Há cancelamento e estados/progresso separados por destino. Consulte o [procedimento de release](ota.md) e os [estados públicos OTA](../firmware/mestre-s3/include/servicos/manutencao/ota.h).

O [protocolo de comandos](../firmware/compartilhado/protocolos/protocolo_comandos.h) permite consultas e ações pelo enlace UART. [`comandos_uart.cpp`](../firmware/mestre-s3/src/servicos/comunicacao/comandos_uart.cpp) mantém o protocolo, e [`comandos_mestre.cpp`](../firmware/mestre-s3/src/servicos/comunicacao/comandos_mestre.cpp) traduz os comandos em consultas e operações dos serviços. [`enlace_uart.cpp`](../firmware/mestre-s3/src/servicos/comunicacao/enlace_uart.cpp) encaminha respostas. O satélite da equipe pode solicitar um retrato compacto e versionado do diagnóstico do mestre. Operações indisponíveis retornam uma resposta explícita.

## Terminal, apresentação e observabilidade

O [terminal serial](../firmware/mestre-s3/src/servicos/interface_local/terminal/console.cpp)
concentra a leitura da entrada, o menu principal e a navegação. Os menus maiores
ficam na mesma pasta, em módulos para Wi-Fi, sensores e operações com OTA e
satélites. Todos usam a mesma tarefa serial e as mesmas rotinas de entrada e
formatação. O menu principal apresenta estas entradas.

| Opção | Ação |
|---:|---|
| 1 | Resumo completo de diagnóstico e tarefas. |
| 11 | Estado rápido: saúde, memória, Wi-Fi, OTA, cartão SD e satélites. |
| 2 | Consultar estado OTA, verificar versão, autorizar mestre/equipe/visitantes/todos, cancelar e ajustar intervalos. |
| 3 / 4 | Configurar intervalos dos relatórios; pausar ou retomar os relatórios periódicos. |
| 5 | Gerenciar redes e política Wi-Fi. |
| 6 | Consultar satélites por ping e versão. |
| 7 | Consultar sensores, odômetro, enlace e falhas. |
| 8 | Enviar CSVs pendentes ao Google. |
| 10 | Ajustar logs por serviço, nível geral e cores do menu. |
| 0 | Redesenhar o menu. |

Em **10**, pode-se mostrar informações, apenas avisos e erros, apenas erros,
ou silenciar todos os logs serial. O perfil **avisos do NEXUS** silencia as
mensagens do ESP-IDF durante a operação e conserva avisos e erros dos serviços
e dispositivos do projeto; outro item restaura os níveis anteriores. Cada
serviço listado pode alternar seu nível individualmente. A escolha é temporária
e não apaga os registros estruturados
no cartão SD. As cores ANSI do menu podem ser desligadas se o monitor serial
não as interpretar. Senhas de redes não mostram caracteres durante a digitação;
somente a senha de permissão das ações protegidas mostra `*`.

Em **7 → 9**, o terminal permite pausar ou retomar individualmente ADC,
MPU6050, encoders, INA228 e sensor interno de temperatura do ESP,
ou todos juntos. O controle é volátil:
na próxima partida, todas as leituras voltam a ser permitidas. A pausa suspende
as chamadas de aquisição, sem desligar a alimentação dos componentes nem a
contagem física do PCNT. Enquanto os encoders estão pausados, a velocidade
publicada é zero **sem indicar que o veículo está parado**, e o percurso fica no último valor; ao retomar, as contagens
acumuladas são incorporadas ao odômetro. O MPU6050 refaz a calibração após a
retomada. Tensão/corrente/potência e dados do MPU não reaproveitam a última
amostra ao serem pausados; falhas anteriores continuam nos contadores, mas um
sensor pausado não mantém o alerta de saúde dos sensores. A temperatura interna deixa de ser
publicada como leitura válida enquanto seu sensor estiver pausado. O MPU6050
e o INA228 têm tentativas de inicialização em serviço após falha de comunicação.
Para os demais sensores, confira o log de partida após corrigir a ligação e
reiniciar. A simulação continua produzindo dados enquanto a aquisição
física estiver pausada; os controles não atuam sobre o veículo.

[`diagnostico.cpp`](../firmware/mestre-s3/src/servicos/manutencao/diagnostico.cpp) reúne contadores de sensores, UART, Wi-Fi, memória, CPU por núcleo, tarefas, menor margem de pilha e causas de saúde. [`relatorios.cpp`](../firmware/mestre-s3/src/servicos/interface_local/relatorios.cpp) controla a apresentação periódica e sob demanda. [`supervisao.cpp`](../firmware/mestre-s3/src/servicos/manutencao/supervisao.cpp) acompanha pulsos e fases; a tarefa de alta prioridade detecta a necessidade de recuperação, enquanto outra tarefa executa ações que podem bloquear. Um atraso na telemetria provoca notificação para antecipar o próximo ciclo, sem apagar a tarefa durante acesso a driver. Os canais UART podem ser recuperados individualmente.

O retrato de diagnóstico remoto solicitado pelo satélite da equipe é compacto
e versionado, com até 128 bytes de carga UART. Inclui CPU por núcleo, tarefa
de maior uso, temperatura interna, RAM/PSRAM, menor margem de pilha, causas
de saúde e contadores de falha. A saúde de memória usa amostras consecutivas
para evitar alarmes por quedas breves durante TLS e OTA; margem mínima de pilha
abaixo de 256 bytes é classificada como falha. Os limiares e prazos detalhados
estão na [arquitetura interna](arquitetura.md).

A [sinalização](../firmware/mestre-s3/src/servicos/interface_local/sinalizacao.cpp) usa quatro LEDs e um buzzer passivo, com tarefa própria de 50 ms: os tons não bloqueiam aquisição, rádio ou gravação. O [painel local](../firmware/mestre-s3/src/servicos/interface_local/painel_oled.cpp) usa SSD1306 de 128×64 pixels, detecta `0x3C`/`0x3D`, apresenta progresso de partida, estados e avisos em fila. Um painel ausente não interrompe a telemetria. Os padrões dos LEDs são:

| Indicação | Significado |
|---|---|
| Pulso azul lento | Inicialização. |
| Verde contínuo | Sistema saudável. |
| Verde contínuo com um pulso azul a cada 3 s | Sessão Wi-Fi ativa. |
| Verde contínuo com dois pulsos azuis a cada 1,5 s | Sincronização com o Google em andamento, se não houver alerta de maior prioridade. |
| Azul piscando lentamente | Consulta OTA em andamento. |
| Azul contínuo com dois pulsos amarelos a cada 3 s | Atualização aguardando autorização. |
| Azul contínuo com um pulso amarelo a cada 1,5 s | Firmware OTA sendo baixado. |
| Verde contínuo com dois pulsos azuis a cada 3 s | Atualização concluída. |
| Um pulso amarelo a cada 2 s | Atenção na saúde do sistema. |
| Amarelo contínuo com um pulso vermelho a cada 3 s | Falha OTA. |
| Vermelho quase contínuo, com breve pausa a cada 2 s | Falha crítica; a pausa distingue o indicador aceso de um LED travado. |

O som identifica **transições**, não cada amostra: duas notas discretas na partida e na confirmação de uma operação, três notas ascendentes quando o sistema fica pronto ou conclui uma OTA; dois tons específicos quando uma OTA fica disponível, começa a instalação ou quando o envio ao Google começa e termina. Avisos e falhas usam notas descendentes. O envio ao Google com erro tem som próprio. Entrar no Wi-Fi e consultar OTA periodicamente não emite som. Uma falha OTA persistente repete o aviso após 2 min; uma falha crítica, após 45 s. Os LEDs continuam mostrando o estado entre avisos sonoros.

Sons de menor prioridade não interrompem falhas críticas, falhas OTA nem a sinalização de uma atualização em curso. O estado de Google é consultado a cada 500 ms pelos contadores de início e conclusão; uma sincronização muito rápida pode produzir apenas o som do resultado. O buzzer é acionado por PWM do periférico LEDC, respeitando o limite de intensidade configurado na placa. Essa intensidade define a razão do sinal PWM e não representa um volume acústico calibrado; o resultado deve ser ouvido e ajustado no buzzer instalado. Se um comando de LED falhar, a tarefa tenta novamente após 500 ms e contabiliza o erro.

Os avisos de partida entram em uma fila fixa e aparecem após a inicialização.
Estados persistentes, como atualização disponível, retornam em janelas curtas;
as páginas normais continuam visíveis entre os lembretes. A conclusão OTA é
numerada pelo serviço, para tela e buzzer perceberem também a atualização de
um satélite quando o estado geral muda rapidamente.

## Configuração e tarefas

Use [`include/nucleo/configuracao`](../firmware/mestre-s3/include/nucleo/configuracao)
para localizar os padrões compilados do mestre. O `main.cpp` coordena a partida e
a composição dos serviços. As credenciais locais ficam em `include/nucleo/segredos_ota.h`.
Os padrões do ESP-IDF ficam em [`sdkconfig.defaults`](../firmware/mestre-s3/sdkconfig.defaults).

| Arquivo | Ajustes principais |
|---|---|
| [`placa.h`](../firmware/mestre-s3/include/nucleo/configuracao/placa.h) | Pinagem, endereços dos dispositivos e parâmetros elétricos da placa, incluindo o barramento do cartão SD. |
| [`veiculo.h`](../firmware/mestre-s3/include/nucleo/configuracao/veiculo.h) | Calibração dos sensores, geometria, limites do odômetro, controle e simulação. |
| [`comunicacao.h`](../firmware/mestre-s3/include/nucleo/configuracao/comunicacao.h) | Parâmetros dos enlaces UART e cadências da telemetria. |
| [`interface.h`](../firmware/mestre-s3/include/nucleo/configuracao/interface.h) | Cadência da interface local e intervalos padrão dos relatórios. |
| [`diagnostico.h`](../firmware/mestre-s3/include/nucleo/configuracao/diagnostico.h) | Limiares de saúde e parâmetros de supervisão. |
| [`tarefas.h`](../firmware/mestre-s3/include/nucleo/configuracao/tarefas.h) | Pilha, prioridade e afinidade das tarefas criadas pelos serviços. |
| [`wifi.h`](../firmware/mestre-s3/include/nucleo/configuracao/wifi.h) | Política e limites do Wi-Fi. |
| [`ota.h`](../firmware/mestre-s3/include/nucleo/configuracao/ota.h) | Manifesto, intervalos e limites da atualização. |
| [`historico.h`](../firmware/mestre-s3/include/nucleo/configuracao/historico.h) | Amostragem, rotação do CSV, logs e envio ao Google. |

Aquisição, telemetria, controle do veículo, enlaces e o pipeline de OTA por UART ficam no núcleo 1. A tarefa principal de OTA, o diagnóstico, o registro e o upload ficam no núcleo 0. Terminal, painel, relatórios, sinalização e supervisão ficam sem afinidade fixa. A [política de tarefas](../firmware/mestre-s3/include/nucleo/configuracao/tarefas.h) define essas afinidades, e o ESP-IDF também executa suas próprias tarefas nos dois núcleos.

O ESP-IDF reúne em [`src/CMakeLists.txt`](../firmware/mestre-s3/src/CMakeLists.txt) o `main.cpp` e as fontes das subpastas de gerenciadores, dispositivos e serviços. O [CMake da raiz](../firmware/mestre-s3/CMakeLists.txt) usa `VERSAO_FIRMWARE` para definir a versão do projeto. O padrão é `0.1.0`.

## Limites e verificações em bancada

### Testes de falha e recuperação

Faça os testes com a tração fisicamente isolada e salve o log do mestre desde a
partida. O console primário do mestre usa 115200 baud. Use um microSD de teste
com cópia dos arquivos que queira preservar.
No menu serial, a opção `1` imprime o resumo `[TAREFAS]` e a opção `11` mostra o
estado rápido do cartão e dos satélites.

| Teste | Resultado esperado |
|---|---|
| Compare um minuto com o painel da equipe fechado e cinco a dez minutos com a aba **Mestre** aberta. Rode consultas aos dois satélites. | `enlace_equipe` e `acoes_mestre` continuam respondendo. Use 1 KiB de menor folga como meta inicial de bancada. Reinicie entre versões porque a folga exibida representa o mínimo desde o boot. |
| Ligue com um microSD FAT32 conhecido e repita a partida sem cartão. Se o soquete admitir inserção em uso, insira o cartão de teste depois da partida. | A telemetria segue ativa. A montagem é retomada na próxima janela de 30 s, mais o tempo de acesso ao cartão. O console indica a frequência usada e as amostras gravadas voltam a subir. |
| Com um cartão de teste e soquete apropriado, simule a perda do cartão e depois reconecte-o. | Após três falhas consecutivas de gravação, o registro programa a remontagem. Um upload em andamento termina antes da desmontagem. O CSV temporário é preservado para recuperação e a gravação retoma após a montagem. |
| Fora de OTA e upload, reinicie só o satélite da Equipe. Use `6 → 1` para ping da Equipe e `6 → 2` para Visitantes. | A consulta à Equipe retorna timeout enquanto o satélite estiver indisponível. Os ACKs voltam após sua partida, Visitantes segue respondendo e o tempo ativo do mestre continua avançando. |
| Compare a partida com e sem MPU6050 ou INA228 em `7 → 5` e `7 → 8`. Altere a fiação com a bancada desligada. Para testar retorno em operação, use um dispositivo de bancada apropriado para isolar alimentação e sinais. | O mestre continua ativo. O MPU6050 retenta em intervalos de 5 s e recalibra após a recuperação. Mantenha o sensor imóvel durante a calibração. O INA228 volta a publicar medições válidas quando recuperado. |
| Consulte pedal e DAC em `12 → 1`. Compare uma partida normal com uma partida com o DAC desconectado. | Com falha de escrita, o DAC passa a indisponível. Ele volta a disponível após nova sondagem e confirmação de escrita em zero. |
| Em bancada, atravesse repetidamente o limite de 30.000 contagens do PCNT enquanto consulta o odômetro e grava no SD. | Distância acumulada monotônica e velocidade sem picos na passagem pelo limite. |

O log `gerenciador_sd` informa os GPIOs, a frequência e o código da tentativa.
Falhas de comunicação na montagem permitem uma repetição a 400 kHz, abaixo
dos 10 MHz normais. Se houver timeout ou CRC, confira primeiro alimentação,
contato, pull-ups e ligação SPI. Se a inicialização do cartão completar e o
log indicar falha em `f_mount`, confira o volume FAT32 num leitor e compare com
outro cartão. O firmware mantém a formatação automática desativada.

O código não comprova a fiação física, calibração mecânica, qualidade de alimentação, presença do cartão ou cobertura da rede. Antes de usar no veículo, confira a relação e o diâmetro de cada roda, orientação do MPU6050, escala do circuito ADC, pull-ups de 3,3 V dos encoders, níveis UART, fonte do buzzer e pinagem SPI. Teste a aquisição com upload e OTA simultâneos, falha de rede, remoção do SD e reinício após gravação do odômetro. A leitura de pilhas, maior bloco livre e logs no SD ajuda a verificar os piores casos de TLS e atualização. Estes são pontos de validação de integração, não resultados de testes já realizados.

## Controle de marcha e acelerador

A Equipe pode configurar o limite da faixa L/M/H, solicitar o sentido frente/ré,
ajustar a velocidade máxima, o teto percentual do pedal, a zona morta e a
tensão mínima da saída DAC e solicitar parada remota. A seleção de faixa vem da
chave física. A aceleração
permanece uma medição, sem limite configurável. O mestre valida as transições
em uma tarefa de 20 ms, prioridade 9, e zera a referência de
acelerador ao receber parada ou perder a leitura local do pedal no GPIO3 por
mais de 300 ms. O MCP4725 recebe 0 V na inicialização, mas sua saída não nula
está desabilitada até validação elétrica; GPIOs de marcha e relé não atuam.
Na partida, o carro permanece desarmado até concluir os serviços, validar os
sensores, confirmar pedal solto por 500 ms, velocidade quase nula e limites
positivos e chave física válida; então o controle arma na marcha da chave,
respeitado o limite remoto. Durante uma instalação OTA, o
carro fica desarmado e, após a instalação, o rearme é manual. Os limites de
velocidade e acelerador são salvos na NVS; a marcha é lida da chave física.
Consulte [Controle de marcha e acelerador](controle-remoto.md)
para estados, bloqueios, prazos, contrato UART/HTTP e pendências de hardware.
