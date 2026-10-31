# Guia técnico do firmware mestre

Este guia descreve o comportamento implementado em `firmware/mestre-s3` no estado atual do código. O [README do firmware](../../firmware/mestre-s3/README.md) apresenta o uso, a [arquitetura](../../firmware/mestre-s3/ARQUITETURA.md) explica a separação entre módulos e a [pinagem geral](../../hardware/pinagem/README.md) detalha as ligações elétricas. Os valores abaixo são **definições de firmware**; a montagem física e os níveis elétricos precisam ser conferidos na bancada.

## Escopo e fluxo de dados

O alvo é o ESP32-S3-WROOM-1-N16R8, com 16 MiB de flash e 8 MiB de PSRAM Octal. O mestre lê tensão por ADC, movimento pelo MPU6050 e deslocamento de dois encoders. A tarefa `telemetria` reúne as leituras em `DadosTelemetriaVeiculo`, publica os quadros para os dois satélites, entrega uma cópia aos relatórios e enfileira amostras para o histórico em cartão SD. A fonte pode ser substituída pelo simulador na compilação. Consulte [a composição em `main.cpp`](../../firmware/mestre-s3/src/main.cpp), [o modelo compartilhado](../../firmware/compartilhado/telemetria/dados_telemetria_veiculo.h) e [a configuração da placa](../../firmware/mestre-s3/include/nucleo/configuracao_placa.h).

| Camada | Responsabilidade | Localização |
|---|---|---|
| `nucleo` | Pinos, limites físicos, tarefas, Wi-Fi, OTA e registro | [`include/nucleo`](../../firmware/mestre-s3/include/nucleo/) |
| `gerenciadores` | Drivers ESP-IDF de UART, I²C, PCNT, SD, Wi-Fi, indicadores e OTA local | [`src/gerenciadores`](../../firmware/mestre-s3/src/gerenciadores/) |
| `dispositivos` | ADC, MPU6050, encoders e controlador SSD1306 | [`src/dispositivos`](../../firmware/mestre-s3/src/dispositivos/) |
| `servicos` | Regras da aplicação, interfaces, supervisão e transferência OTA | [`src/servicos`](../../firmware/mestre-s3/src/servicos/) |
| `main.cpp` | Política de partida, tolerância a falhas e ciclo de aquisição | [`src/main.cpp`](../../firmware/mestre-s3/src/main.cpp) |

## Pinout do mestre

Fonte canônica dos GPIOs: [`configuracao_placa.h`](../../firmware/mestre-s3/include/nucleo/configuracao_placa.h) e [`configuracao_registro_telemetria.h`](../../firmware/mestre-s3/include/nucleo/configuracao_registro_telemetria.h). O barramento UART usa 3,3 V, 8N1 e 1.000.000 baud conforme [`configuracao_enlace_uart.h`](../../firmware/compartilhado/protocolos/configuracao_enlace_uart.h). Mestre e satélites compartilham GND.

| Função | Interface | GPIO mestre | Observação |
|---|---|---|---|
| Leitura de tensão | ADC | 4 | A entrada deve respeitar o limite do ADC; o condicionamento externo é necessário para tensões maiores. |
| Encoder esquerdo A/B | PCNT | 5 / 6 | Saídas NPN exigem pull-ups externos para **3,3 V**. |
| Encoder direito A/B | PCNT | 7 / 21 | Mesma exigência de pull-ups. |
| MPU6050 e SSD1306 SDA/SCL | I²C0 | 8 / 9 | 400 kHz; MPU em `0x68`, tela em `0x3C` ou `0x3D`. |
| LEDs verde/amarelo/vermelho/azul | GPIO | 10 / 11 / 12 / 13 | Ativos em nível alto, cada LED com resistor em série. |
| Buzzer passivo | PWM/LEDC | 14 | Acionamento externo recomendado na pinagem geral. |
| UART2 visitantes TX/RX | UART | 15 / 16 | Cruzar TX com RX do satélite. |
| UART1 equipe TX/RX | UART | 17 / 18 | Cruzar TX com RX do satélite. |
| microSD CLK/MOSI/MISO/CS | SPI2 | 38 / 39 / 40 / 41 | Módulo alimentado em 3,3 V; frequência configurada de 10 MHz. |
| INMP441 BCLK/WS/SD | I²S0 | 42 / 47 / 48 | Recurso temporário de gravação; L/R do microfone em GND. |

Os GPIOs 33 a 37 ficam reservados para a PSRAM Octal do módulo N16R8. O INMP441, o SD e o barramento de encoders devem ser validados no hardware montado; a tabela registra a configuração de software. A [pinagem geral](../../hardware/pinagem/README.md) inclui recomendações de alimentação e ligação.

## Partida, degradação e recuperação

[`app_main`](../../firmware/mestre-s3/src/main.cpp) verifica primeiro a flash de 16 MiB. Flash incompatível interrompe a partida; PSRAM ausente ou com tamanho inesperado gera aviso e os recursos opcionais tentam RAM interna. Inicializa sinalização e tela, depois NVS, supervisão, enlaces e aquisição, registro SD, tarefa de telemetria, Wi-Fi, OTA, terminal, relatórios e diagnóstico. A NVS não é apagada automaticamente quando precisa de manutenção, pois contém odômetro e perfis Wi-Fi.

Falhas de NVS, inicialização do núcleo da aplicação, criação/supervisão da tarefa de telemetria ou confirmação da imagem OTA pendente provocam rejeição da imagem e reinício. Falhas isoladas de ADC, MPU6050, odômetro, SD, tela, LEDs, Wi-Fi, terminal, relatórios e diagnóstico permitem operação degradada com aviso. Sem cartão, a telemetria ao vivo continua e o registro é tentado novamente a cada 30 s. Uma imagem OTA ainda pendente só é confirmada depois que Wi-Fi e serviço OTA passam pela inicialização necessária à recuperação remota. Fonte: [`main.cpp`](../../firmware/mestre-s3/src/main.cpp).

## Aquisição e telemetria

`USAR_TELEMETRIA_SIMULADA=false` seleciona os sensores reais atualmente; `true` usa [`servico_simulador_telemetria.cpp`](../../firmware/mestre-s3/src/servicos/servico_simulador_telemetria.cpp). O ciclo normal é de 250 ms (4 Hz). ADC indisponível, leitura MPU inválida e falha no odômetro são contabilizados ou sinalizados; uma falha pontual não derruba o restante do ciclo. O MPU é amostrado por serviço próprio a 100 Hz, com calibração, compensação de gravidade, filtro e histerese para o sentido longitudinal. A montagem codificada assume X vertical e Y longitudinal positivo para frente. Fontes: [`main.cpp`](../../firmware/mestre-s3/src/main.cpp), [`servico_aceleracao.cpp`](../../firmware/mestre-s3/src/servicos/servico_aceleracao.cpp).

Os dois encoders de 600 P/R são decodificados pelo PCNT em quatro transições por pulso (2.400 contagens/giro). [`servico_odometro.cpp`](../../firmware/mestre-s3/src/servicos/servico_odometro.cpp) aplica diâmetro de 0,300 m e relação encoder/roda de 1,0 em cada lado, usa a média das distâncias para velocidade e distância, rejeita saltos acima do limite configurado de 200 km/h e acompanha divergência persistente entre rodas. `percurso_km` recomeça no boot; `odometro_km` é recuperado da NVS e persistido a cada 100 m. Ajuste esses valores à mecânica real em [`configuracao_placa.h`](../../firmware/mestre-s3/include/nucleo/configuracao_placa.h).

[`servico_telemetria.cpp`](../../firmware/mestre-s3/src/servicos/servico_telemetria.cpp) envia o mesmo retrato a equipe e visitantes, com fila de transmissão por UART. O [protocolo v4](../../firmware/compartilhado/protocolos/protocolo_telemetria.h) define pacote de 105 bytes, carga de 98 bytes, CRC-16/CCITT-FALSE e confirmação de 20 bytes. Os retornos permitem medir RTT e idade da última resposta de cada satélite. [`servico_enlace_satelites.cpp`](../../firmware/mestre-s3/src/servicos/servico_enlace_satelites.cpp) é o consumidor dos quadros de retorno e os encaminha para telemetria, comandos ou OTA. O [parser do enlace](../../firmware/compartilhado/protocolos/protocolo_enlace_uart.h) distingue as três famílias de quadro.

Durante OTA de satélite, o intervalo da telemetria passa, por padrão, a 2 s e pode ser alterado entre 500 ms e 10 s; a mudança mantém um canal de observação enquanto a UART transporta o firmware. Fonte: [`configuracao_placa.h`](../../firmware/mestre-s3/include/nucleo/configuracao_placa.h), [`servico_telemetria.cpp`](../../firmware/mestre-s3/src/servicos/servico_telemetria.cpp).

## Armazenamento e envio ao Google

[`servico_registro_telemetria.cpp`](../../firmware/mestre-s3/src/servicos/servico_registro_telemetria.cpp) reduz o retrato ao vivo para 1 amostra/s e grava CSV em `/sdcard/telemetria`. O arquivo ativo é sincronizado a cada 10 s e gira após 1 h ou 2 MiB. Uma fila de 24 amostras desacopla aquisição e escrita. O envio pendente é solicitado pelo usuário no terminal; abre sessão Wi-Fi, transmite o CSV em blocos e só o move para `/sdcard/telemetria/enviados` após confirmação do destino. Falhas preservam o arquivo para nova tentativa. O serviço também mantém `/sdcard/logs/esp_<sessao>_<parte>.log` com diagnóstico periódico a cada 30 s, segmentos em torno de 1 MiB e limite de cerca de 8 MiB por sessão; esses logs não são enviados ao Google. Consulte [telemetria no Google](../telemetria-google/README.md) para o Web App e as credenciais.

A opção temporária de áudio lê o INMP441 e cria `/sdcard/audio_ia/<nome>.wav` mono PCM 16 bits a 16 kHz, com 1 a 60 s; arquivos existentes não são sobrescritos. Ela usa o cartão SD e recusa gravação quando Wi-Fi, OTA ou upload estão ativos. Fontes: [`servico_gravador_audio_treinamento.cpp`](../../firmware/mestre-s3/src/servicos/servico_gravador_audio_treinamento.cpp), [`servico_terminal.cpp`](../../firmware/mestre-s3/src/servicos/servico_terminal.cpp).

## Wi-Fi, OTA e comandos

O rádio opera como estação sob sessões temporárias. [`servico_wifi.cpp`](../../firmware/mestre-s3/src/servicos/servico_wifi.cpp) tenta primeiro a rede principal, administra até cinco redes secundárias salvas na NVS e exige autorização para tentativas secundárias salvo quando o modo automático foi habilitado. Trocar/restaurar rede principal ou mudar o modo automático requer `WIFI_SENHA_PERMISSAO` (com fallback para `OTA_WIFI_SENHA`). Canal preferencial, largura e potência estão em [`configuracao_wifi.h`](../../firmware/mestre-s3/include/nucleo/configuracao_wifi.h). Senhas privadas pertencem ao arquivo local `include/nucleo/segredos_ota.h`, criado a partir do exemplo e ignorado pelo Git.

[`servico_ota.cpp`](../../firmware/mestre-s3/src/servicos/servico_ota.cpp) verifica por padrão a cada 15 minutos um manifesto de GitHub Release por HTTPS; download exige autorização no terminal. O manifesto e cada imagem são verificados antes de aplicar. A atualização do mestre usa partições OTA e rollback; nos satélites, [`transporte_ota_satelites.cpp`](../../firmware/mestre-s3/src/servicos/transporte_ota_satelites.cpp) faz download HTTPS e envio UART confirmado em pipeline com três blocos reutilizáveis de 4 KiB. O fluxo "todos" atualiza os satélites antes do mestre. Há cancelamento e estados/progresso separados por destino. Consulte o [procedimento de release](../ota/README.md) e os [estados públicos OTA](../../firmware/mestre-s3/include/servicos/servico_ota.h).

O [protocolo de comandos](../../firmware/compartilhado/protocolos/protocolo_comandos.h) permite consultas e ações pelo enlace UART. [`servico_comandos.cpp`](../../firmware/mestre-s3/src/servicos/servico_comandos.cpp) executa operações do mestre e [`servico_enlace_satelites.cpp`](../../firmware/mestre-s3/src/servicos/servico_enlace_satelites.cpp) encaminha respostas. O satélite da equipe pode solicitar um retrato compacto e versionado do diagnóstico do mestre. As capacidades são anunciadas por bits; a presença de um identificador no protocolo não garante que todo alvo implemente a ação.

## Terminal, apresentação e observabilidade

O [menu serial](../../firmware/mestre-s3/src/servicos/servico_terminal.cpp) apresenta estas entradas de nível superior:

| Opção | Ação |
|---:|---|
| 1 | Resumo completo de diagnóstico e tarefas. |
| 2 | Consultar estado OTA, verificar versão, autorizar mestre/equipe/visitantes/todos, cancelar e ajustar intervalos. |
| 3 / 4 | Configurar intervalos dos relatórios; pausar ou retomar os relatórios periódicos. |
| 5 | Gerenciar redes e política Wi-Fi. |
| 6 | Consultar satélites por ping e versão. |
| 7 | Consultar sensores, odômetro, enlace e falhas. |
| 8 | Enviar CSVs pendentes ao Google. |
| 9 | Gravar WAV temporário para treinamento. |
| 0 | Redesenhar o menu. |

[`servico_diagnostico.cpp`](../../firmware/mestre-s3/src/servicos/servico_diagnostico.cpp) reúne contadores de sensores, UART, Wi-Fi, memória, CPU por núcleo, tarefas, menor margem de pilha e causas de saúde. [`servico_relatorios.cpp`](../../firmware/mestre-s3/src/servicos/servico_relatorios.cpp) controla a apresentação periódica e sob demanda. [`servico_supervisao.cpp`](../../firmware/mestre-s3/src/servicos/servico_supervisao.cpp) acompanha pulsos e fases; a tarefa de alta prioridade detecta a necessidade de recuperação, enquanto outra tarefa executa ações que podem bloquear. Um atraso na telemetria provoca notificação para antecipar o próximo ciclo, sem apagar a tarefa durante acesso a driver. Os canais UART podem ser recuperados individualmente.

A [sinalização](../../firmware/mestre-s3/src/servicos/servico_sinalizacao.cpp) mapeia partida, saúde, Wi-Fi e OTA para quatro LEDs e melodias no buzzer sem bloquear tarefas. O [painel local](../../firmware/mestre-s3/src/servicos/servico_painel_local.cpp) usa SSD1306 de 128×64 pixels, detecta `0x3C`/`0x3D`, apresenta progresso de partida, estados e avisos em fila. Um painel ausente não interrompe a telemetria. As cores e os padrões estão descritos no [README do firmware](../../firmware/mestre-s3/README.md).

## Configuração e tarefas

| Arquivo | Ajustes principais |
|---|---|
| [`configuracao_placa.h`](../../firmware/mestre-s3/include/nucleo/configuracao_placa.h) | GPIOs, aquisição, geometria, limites dos encoders, simulador e taxa UART/telemetria. |
| [`configuracao_tarefas.h`](../../firmware/mestre-s3/include/nucleo/configuracao_tarefas.h) | Nome, pilha, prioridade e afinidade das tarefas criadas pelos serviços. |
| [`configuracao_wifi.h`](../../firmware/mestre-s3/include/nucleo/configuracao_wifi.h) | Perfis, credenciais padrão e limites da política Wi-Fi. |
| [`configuracao_ota.h`](../../firmware/mestre-s3/include/nucleo/configuracao_ota.h) | Manifesto, intervalos e limites HTTP/OTA. |
| [`configuracao_registro_telemetria.h`](../../firmware/mestre-s3/include/nucleo/configuracao_registro_telemetria.h) | SPI, CSV, logs e destino Google. |
| [`sdkconfig.defaults`](../../firmware/mestre-s3/sdkconfig.defaults) | Flash QIO 16 MB, PSRAM Octal, certificados, rollback, pilhas do sistema e estatísticas FreeRTOS. |

As tarefas de aquisição, telemetria e enlaces ficam no núcleo 1; OTA, CSV e upload no núcleo 0; terminal, painel, relatórios, diagnóstico, sinalização e supervisão ficam sem afinidade fixa conforme a [política de tarefas](../../firmware/mestre-s3/include/nucleo/configuracao_tarefas.h). Essa tabela registra a configuração, não reserva os núcleos para uso exclusivo. O projeto compila com ESP-IDF por [`CMakeLists.txt`](../../firmware/mestre-s3/CMakeLists.txt); `VERSAO_FIRMWARE` define a versão do projeto e, quando ausente, o padrão é `0.1.0`.

## Limites e verificações em bancada

O código não comprova a fiação física, calibração mecânica, qualidade de alimentação, presença do cartão ou cobertura da rede. Antes de usar no veículo, confira a relação e o diâmetro de cada roda, orientação do MPU6050, escala do circuito ADC, pull-ups de 3,3 V dos encoders, níveis UART, fonte do buzzer e pinagem SPI/I²S. Teste a aquisição com upload e OTA simultâneos, falha de rede, remoção do SD e reinício após gravação do odômetro. A leitura de pilhas, maior bloco livre e logs no SD ajuda a verificar os piores casos de TLS e atualização. Estes são pontos de validação de integração, não resultados de testes já realizados.
