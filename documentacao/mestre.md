# Guia técnico do firmware mestre

[Índice da documentação](README.md)

Este guia descreve o comportamento implementado em `firmware/mestre-s3` no estado atual do código. A [arquitetura](arquitetura.md) explica a separação entre módulos e a [pinagem geral](pinagem.md) detalha as ligações elétricas. Os valores abaixo são **definições de firmware**; a montagem física e os níveis elétricos precisam ser conferidos na bancada.

## Escopo e fluxo de dados

O alvo é o ESP32-S3-WROOM-1-N16R8, com 16 MiB de flash e 8 MiB de PSRAM Octal. O mestre lê tensão por ADC, movimento pelo MPU6050 e deslocamento de dois encoders. A tarefa `telemetria` reúne as leituras em `DadosTelemetriaVeiculo`, publica os quadros para os dois satélites, entrega uma cópia aos relatórios e enfileira amostras para o histórico em cartão SD. A fonte padrão é definida no firmware e pode ser trocada temporariamente pelo painel da equipe. Consulte [a composição em `main.cpp`](../firmware/mestre-s3/src/main.cpp), [o modelo compartilhado](../firmware/compartilhado/telemetria/dados_telemetria_veiculo.h) e [a configuração da placa](../firmware/mestre-s3/include/nucleo/configuracao_placa.h).

| Camada | Responsabilidade | Localização |
|---|---|---|
| `nucleo` | Pinos, limites físicos, tarefas, Wi-Fi, OTA e registro | [`include/nucleo`](../firmware/mestre-s3/include/nucleo) |
| `gerenciadores` | Drivers ESP-IDF de UART, I²C, PCNT, SD, Wi-Fi, indicadores e OTA local | [`src/gerenciadores`](../firmware/mestre-s3/src/gerenciadores) |
| `dispositivos` | ADC, MPU6050, encoders e controlador SSD1306 | [`src/dispositivos`](../firmware/mestre-s3/src/dispositivos) |
| `servicos` | Regras da aplicação, interfaces, supervisão e transferência OTA | [`src/servicos`](../firmware/mestre-s3/src/servicos) |
| `main.cpp` | Política de partida, tolerância a falhas e ciclo de aquisição | [`src/main.cpp`](../firmware/mestre-s3/src/main.cpp) |

### Mapa dos módulos ativos

| Grupo | Arquivos em `firmware/mestre-s3/src` | Responsabilidade |
|---|---|---|
| Entrada | `main.cpp` | Inicialização, composição dos serviços e ciclo principal. |
| Gerenciadores | `gerenciador_uart`, `gerenciador_i2c`, `gerenciador_contador_pulsos` | Drivers dos enlaces e barramentos de aquisição. |
| Gerenciadores | `gerenciador_cartao_sd`, `gerenciador_wifi`, `gerenciador_ota`, `gerenciador_indicadores` | FAT/SPI, rádio, partição OTA local, LEDs e PWM. |
| Dispositivos | `leitor_tensao`, `mpu6050`, `encoder_incremental`, `tela_ssd1306` | ADC bruto e regras específicas dos componentes físicos. |
| Aquisição | `servico_aceleracao`, `servico_odometro`, `servico_telemetria`, `servico_simulador_telemetria` | Filtragem do MPU, geometria das rodas, quadro ao vivo e fonte simulada. |
| Enlaces | `servico_enlace_satelites`, `servico_comandos` | Recepção de ACK/respostas e execução de consultas ou ações. |
| Armazenamento | `servico_registro_telemetria`, `servico_gravador_audio_treinamento` | CSV/logs/upload e WAV temporário para treinamento. |
| Rede/atualização | `servico_wifi`, `servico_ota`, `transporte_ota_satelites` | Política de conexão, orquestração OTA e transferência HTTPS/UART. |
| Observabilidade | `servico_diagnostico`, `servico_supervisao`, `servico_relatorios` | Saúde, recuperação de tarefas/enlaces e apresentação de relatórios. |
| Interface local | `servico_terminal`, `servico_painel_local`, `servico_sinalizacao` | Menu serial, OLED, LEDs e buzzer. |

Os cabeçalhos correspondentes ficam em `include/`; os valores de configuração
ficam em `include/nucleo`. Os detalhes de dependência e recuperação estão na
[arquitetura](arquitetura.md).

## Pinout do mestre

Fonte canônica dos GPIOs: [`configuracao_placa.h`](../firmware/mestre-s3/include/nucleo/configuracao_placa.h) e [`configuracao_registro_telemetria.h`](../firmware/mestre-s3/include/nucleo/configuracao_registro_telemetria.h). O barramento UART usa 3,3 V, 8N1 e 1.000.000 baud conforme [`configuracao_enlace_uart.h`](../firmware/compartilhado/protocolos/configuracao_enlace_uart.h). Mestre e satélites compartilham GND.

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

Os GPIOs 33 a 37 ficam reservados para a PSRAM Octal do módulo N16R8. O INMP441, o SD e o barramento de encoders devem ser validados no hardware montado; a tabela registra a configuração de software. A [pinagem geral](pinagem.md) inclui recomendações de alimentação e ligação.

## Partida, degradação e recuperação

[`app_main`](../firmware/mestre-s3/src/main.cpp) verifica primeiro a flash de 16 MiB. Flash incompatível interrompe a partida; PSRAM ausente ou com tamanho inesperado gera aviso e os recursos opcionais tentam RAM interna. Inicializa sinalização e tela, depois NVS, supervisão, enlaces e aquisição, registro SD, tarefa de telemetria, Wi-Fi, OTA, terminal, relatórios e diagnóstico. A NVS não é apagada automaticamente quando precisa de manutenção, pois contém odômetro e perfis Wi-Fi.

Falhas de NVS, inicialização do núcleo da aplicação, criação/supervisão da tarefa de telemetria ou confirmação da imagem OTA pendente provocam rejeição da imagem e reinício. Falhas isoladas de ADC, MPU6050, odômetro, SD, tela, LEDs, Wi-Fi, terminal, relatórios e diagnóstico permitem operação degradada com aviso. Sem cartão, a telemetria ao vivo continua e o registro é tentado novamente a cada 30 s. Uma imagem OTA pendente permanece em observação por 2 min depois de os serviços iniciarem; reinício antes da confirmação aciona o rollback do bootloader. Até 5 min após o primeiro boot, três reinícios por panic ou watchdog também solicitam rollback. A contagem usa memória RTC e se perde após corte total de energia. Fonte: [`main.cpp`](../firmware/mestre-s3/src/main.cpp) e [`guarda_rollback_ota.h`](../firmware/compartilhado/ota/guarda_rollback_ota.h).

## Aquisição e telemetria

`USAR_TELEMETRIA_SIMULADA=false` seleciona os sensores reais na partida; `true` usa [`servico_simulador_telemetria.cpp`](../firmware/mestre-s3/src/servicos/servico_simulador_telemetria.cpp). O painel da equipe permite ativar ou desativar a simulação durante a sessão; ao reiniciar, retorna ao valor definido no código. A consulta remota informa fonte atual, padrão, dinâmica e amostras geradas. O ciclo normal é de 250 ms (4 Hz). ADC indisponível, leitura MPU inválida e falha no odômetro são contabilizados ou sinalizados; uma falha pontual não derruba o restante do ciclo. O MPU é amostrado por serviço próprio a 100 Hz, com calibração, compensação de gravidade, filtro e histerese para o sentido longitudinal. A montagem codificada assume X vertical e Y longitudinal positivo para frente. Fontes: [`main.cpp`](../firmware/mestre-s3/src/main.cpp), [`servico_aceleracao.cpp`](../firmware/mestre-s3/src/servicos/servico_aceleracao.cpp).

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

Os dois encoders de 600 P/R são decodificados pelo PCNT em quatro transições por pulso (2.400 contagens/giro). [`servico_odometro.cpp`](../firmware/mestre-s3/src/servicos/servico_odometro.cpp) aplica diâmetro de 0,300 m e relação encoder/roda de 1,0 em cada lado, usa a média das distâncias para velocidade e distância, rejeita saltos acima do limite configurado de 200 km/h e acompanha divergência persistente entre rodas. `percurso_km` recomeça no boot; `odometro_km` é recuperado da NVS e persistido a cada 100 m. Ajuste esses valores à mecânica real em [`configuracao_placa.h`](../firmware/mestre-s3/include/nucleo/configuracao_placa.h).

[`servico_telemetria.cpp`](../firmware/mestre-s3/src/servicos/servico_telemetria.cpp) envia o mesmo retrato a equipe e visitantes, com fila de transmissão por UART. O [protocolo v4](../firmware/compartilhado/protocolos/protocolo_telemetria.h) define pacote de 105 bytes, carga de 98 bytes, CRC-16/CCITT-FALSE e confirmação de 20 bytes. Os retornos permitem medir RTT e idade da última resposta de cada satélite. [`servico_enlace_satelites.cpp`](../firmware/mestre-s3/src/servicos/servico_enlace_satelites.cpp) é o consumidor dos quadros de retorno e os encaminha para telemetria, comandos ou OTA. O [parser do enlace](../firmware/compartilhado/protocolos/protocolo_enlace_uart.h) distingue as três famílias de quadro.

Durante OTA de satélite, o intervalo da telemetria passa, por padrão, a 2 s e pode ser alterado entre 500 ms e 10 s; a mudança mantém um canal de observação enquanto a UART transporta o firmware. Fonte: [`configuracao_placa.h`](../firmware/mestre-s3/include/nucleo/configuracao_placa.h), [`servico_telemetria.cpp`](../firmware/mestre-s3/src/servicos/servico_telemetria.cpp).

## Armazenamento e envio ao Google

[`servico_registro_telemetria.cpp`](../firmware/mestre-s3/src/servicos/servico_registro_telemetria.cpp) reduz o retrato ao vivo para 1 amostra/s e grava CSV em `/sdcard/telemetria`. O arquivo ativo é sincronizado a cada 10 s e gira após 1 h ou 8 MiB, mantendo a mesma quantidade de amostras com menos arquivos. Uma fila de 24 amostras desacopla aquisição e escrita. O envio pendente é solicitado pelo usuário no terminal ou painel da equipe; abre sessão Wi-Fi, transmite o CSV em blocos, tolera escritas HTTPS parciais e só move o arquivo para `/sdcard/telemetria/enviados` após confirmação nominal do Google. Falhas preservam o arquivo para nova tentativa e registram o resultado HTTP quando disponível.

O serviço mantém `/sdcard/logs/esp_00000001.log`, `esp_00000002.log` e assim por diante, sem reiniciar a numeração a cada sessão. Cada arquivo comporta cerca de 8 MiB, com limite de 16 MiB de log por sessão. Em operação normal, grava um resumo relevante a cada 30 s. Uma tarefa independente conserva na PSRAM as últimas 120 amostras de saúde, memória, erros, tarefas críticas e estado/latência dos satélites, a 1 Hz. Sem PSRAM, usa um histórico reduzido de 30 amostras na RAM interna. Na falha detectada, grava uma vez o histórico anterior e o contexto atual (`HIST_INICIO`, `HIST`, `HIST_FIM`), com identificação do incidente; depois usa intervalo de 2 s por 2 min ou, em falha grave, de 1 s por 5 min. Novos erros podem antecipar um registro, respeitando o limite de um por segundo. Uma nova falha grave pode antecipar outra captura. A limitação de capturas evita repetir o mesmo histórico continuamente. Se o SD estiver indisponível, mantém o primeiro lote em RAM para nova tentativa e conta incidentes adicionais que não couberem; esse histórico volátil se perde se o ESP desligar antes da gravação. `TEMPO` informa o tempo desde o boot e UTC quando um cabeçalho `Date` válido é recebido por HTTPS na consulta OTA ou no envio ao Google. Antes disso, a ordem dos arquivos identifica qual é mais recente. O log do SD é um diagnóstico estruturado; as linhas individuais `ESP_LOG*` continuam no console serial. Os logs não são enviados ao Google.

`/sdcard/configuracao_atual.txt` guarda um retrato com sequência e checksum, regravado até a cada 60 s por arquivo temporário recuperável. Inclui odômetro, nomes das redes sem senhas, política Wi-Fi, intervalos OTA e de relatórios e parâmetros do CSV. Se a chave do odômetro desaparecer da NVS, o firmware restaura automaticamente esse valor do SD; os demais campos servem para recuperação manual. Um corte de energia pode deixar até cerca de 1 min de diferença entre o odômetro real e o último retrato do SD. Senhas e outras credenciais não são armazenadas nesse arquivo. Consulte [telemetria no Google](google.md) para o Web App e as credenciais.

A opção temporária de áudio lê o INMP441 e cria `/sdcard/audio_ia/<nome>.wav` mono PCM 16 bits a 16 kHz, com 1 a 60 s; arquivos existentes não são sobrescritos. Ela usa o cartão SD e recusa gravação quando Wi-Fi, OTA ou upload estão ativos. Fontes: [`servico_gravador_audio_treinamento.cpp`](../firmware/mestre-s3/src/servicos/servico_gravador_audio_treinamento.cpp), [`servico_terminal.cpp`](../firmware/mestre-s3/src/servicos/servico_terminal.cpp).

## Wi-Fi, OTA e comandos

O rádio opera como estação sob sessões temporárias. [`servico_wifi.cpp`](../firmware/mestre-s3/src/servicos/servico_wifi.cpp) tenta primeiro a rede principal, administra até cinco redes secundárias salvas na NVS e exige autorização para tentativas secundárias salvo quando o modo automático foi habilitado. Trocar/restaurar rede principal ou mudar o modo automático requer `WIFI_SENHA_PERMISSAO` (com fallback para `OTA_WIFI_SENHA`). Canal preferencial, largura e potência estão em [`configuracao_wifi.h`](../firmware/mestre-s3/include/nucleo/configuracao_wifi.h). Senhas privadas pertencem ao arquivo local `include/nucleo/segredos_ota.h`, criado a partir do exemplo e ignorado pelo Git.

No menu serial, a opção 5 permite testar uma rede informada, salvá-la, conectar
uma rede salva ou excluí-la. A rede principal é tentada primeiro; redes
secundárias exigem confirmação, salvo no modo automático protegido por senha.
As credenciais e preferências ficam na NVS e não aparecem em relatórios ou logs.
`WIFI_CANAL_PREFERENCIAL` vale somente para o início da busca em modo estação:
o roteador determina o canal após a associação, e `0` aceita qualquer canal.
`WIFI_LARGURA_CANAL_MHZ` e `WIFI_POTENCIA_MAXIMA_DBM` definem os outros limites
do rádio. As funções públicas de `servico_wifi.h` aplicam a mesma política
quando chamadas por serviços diferentes do terminal.

[`servico_ota.cpp`](../firmware/mestre-s3/src/servicos/servico_ota.cpp) verifica por padrão a cada 15 minutos um manifesto de GitHub Release por HTTPS; download exige autorização no terminal ou painel da equipe. Uma oferta sem autorização expira em 5 min e volta ao estado de cancelamento. O manifesto e cada imagem são verificados antes de aplicar. A atualização do mestre usa partições OTA e rollback; nos satélites, [`transporte_ota_satelites.cpp`](../firmware/mestre-s3/src/servicos/transporte_ota_satelites.cpp) faz download HTTPS e envio UART confirmado em pipeline com três blocos reutilizáveis de 4 KiB. O fluxo "todos" atualiza os satélites antes do mestre. Há cancelamento e estados/progresso separados por destino. Consulte o [procedimento de release](ota.md) e os [estados públicos OTA](../firmware/mestre-s3/include/servicos/servico_ota.h).

O [protocolo de comandos](../firmware/compartilhado/protocolos/protocolo_comandos.h) permite consultas e ações pelo enlace UART. [`servico_comandos.cpp`](../firmware/mestre-s3/src/servicos/servico_comandos.cpp) executa operações do mestre e [`servico_enlace_satelites.cpp`](../firmware/mestre-s3/src/servicos/servico_enlace_satelites.cpp) encaminha respostas. O satélite da equipe pode solicitar um retrato compacto e versionado do diagnóstico do mestre. Operações indisponíveis retornam uma resposta explícita.

## Terminal, apresentação e observabilidade

O [menu serial](../firmware/mestre-s3/src/servicos/servico_terminal.cpp) apresenta estas entradas de nível superior:

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

[`servico_diagnostico.cpp`](../firmware/mestre-s3/src/servicos/servico_diagnostico.cpp) reúne contadores de sensores, UART, Wi-Fi, memória, CPU por núcleo, tarefas, menor margem de pilha e causas de saúde. [`servico_relatorios.cpp`](../firmware/mestre-s3/src/servicos/servico_relatorios.cpp) controla a apresentação periódica e sob demanda. [`servico_supervisao.cpp`](../firmware/mestre-s3/src/servicos/servico_supervisao.cpp) acompanha pulsos e fases; a tarefa de alta prioridade detecta a necessidade de recuperação, enquanto outra tarefa executa ações que podem bloquear. Um atraso na telemetria provoca notificação para antecipar o próximo ciclo, sem apagar a tarefa durante acesso a driver. Os canais UART podem ser recuperados individualmente.

O retrato de diagnóstico remoto solicitado pelo satélite da equipe é compacto
e versionado, com até 128 bytes de carga UART. Inclui CPU por núcleo, tarefa
de maior uso, temperatura interna, RAM/PSRAM, menor margem de pilha, causas
de saúde e contadores de falha. A saúde de memória usa amostras consecutivas
para evitar alarmes por quedas breves durante TLS e OTA; margem mínima de pilha
abaixo de 256 bytes é classificada como falha. Os limiares e prazos detalhados
estão na [arquitetura interna](arquitetura.md).

A [sinalização](../firmware/mestre-s3/src/servicos/servico_sinalizacao.cpp) mapeia partida, saúde, Wi-Fi e OTA para quatro LEDs e melodias no buzzer sem bloquear tarefas. O [painel local](../firmware/mestre-s3/src/servicos/servico_painel_local.cpp) usa SSD1306 de 128×64 pixels, detecta `0x3C`/`0x3D`, apresenta progresso de partida, estados e avisos em fila. Um painel ausente não interrompe a telemetria. Os padrões estão na tabela abaixo.

| Indicação | Significado |
|---|---|
| Azul e amarelo em animação | Inicialização. |
| Verde contínuo | Sistema saudável. |
| Verde com pulso azul duplo | Sessão Wi-Fi ativa. |
| Azul e amarelo alternados | Verificação OTA. |
| Azul contínuo com pulso amarelo duplo | Atualização aguardando autorização. |
| Azul com animação verde e amarela | Firmware OTA sendo baixado. |
| Sequência verde, azul e amarela | Atualização concluída. |
| Amarelo duplo | Atenção na saúde do sistema. |
| Amarelo contínuo com vermelho duplo | Falha OTA; alarme a cada 15 s. |
| Vermelho em SOS | Falha crítica; alarme a cada 8 s. |

Os avisos de partida entram em uma fila fixa e aparecem após a inicialização.
Estados persistentes, como atualização disponível, retornam em janelas curtas;
as páginas normais continuam visíveis entre os lembretes. A conclusão OTA é
numerada pelo serviço, para tela e buzzer perceberem também a atualização de
um satélite quando o estado geral muda rapidamente.

## Configuração e tarefas

| Arquivo | Ajustes principais |
|---|---|
| [`configuracao_placa.h`](../firmware/mestre-s3/include/nucleo/configuracao_placa.h) | GPIOs, aquisição, geometria, limites dos encoders, simulador e taxa UART/telemetria. |
| [`configuracao_tarefas.h`](../firmware/mestre-s3/include/nucleo/configuracao_tarefas.h) | Nome, pilha, prioridade e afinidade das tarefas criadas pelos serviços. |
| [`configuracao_wifi.h`](../firmware/mestre-s3/include/nucleo/configuracao_wifi.h) | Perfis, credenciais padrão e limites da política Wi-Fi. |
| [`configuracao_ota.h`](../firmware/mestre-s3/include/nucleo/configuracao_ota.h) | Manifesto, intervalos e limites HTTP/OTA. |
| [`configuracao_registro_telemetria.h`](../firmware/mestre-s3/include/nucleo/configuracao_registro_telemetria.h) | SPI, CSV, logs e destino Google. |
| [`sdkconfig.defaults`](../firmware/mestre-s3/sdkconfig.defaults) | Flash QIO 16 MB, PSRAM Octal, certificados, rollback, pilhas do sistema e estatísticas FreeRTOS. |

As tarefas de aquisição, telemetria e enlaces ficam no núcleo 1; OTA, CSV e upload no núcleo 0; terminal, painel, relatórios, diagnóstico, sinalização e supervisão ficam sem afinidade fixa conforme a [política de tarefas](../firmware/mestre-s3/include/nucleo/configuracao_tarefas.h). Essa tabela registra a configuração, não reserva os núcleos para uso exclusivo. O projeto compila com ESP-IDF por [`CMakeLists.txt`](../firmware/mestre-s3/CMakeLists.txt); `VERSAO_FIRMWARE` define a versão do projeto e, quando ausente, o padrão é `0.1.0`.

## Limites e verificações em bancada

O código não comprova a fiação física, calibração mecânica, qualidade de alimentação, presença do cartão ou cobertura da rede. Antes de usar no veículo, confira a relação e o diâmetro de cada roda, orientação do MPU6050, escala do circuito ADC, pull-ups de 3,3 V dos encoders, níveis UART, fonte do buzzer e pinagem SPI/I²S. Teste a aquisição com upload e OTA simultâneos, falha de rede, remoção do SD e reinício após gravação do odômetro. A leitura de pilhas, maior bloco livre e logs no SD ajuda a verificar os piores casos de TLS e atualização. Estes são pontos de validação de integração, não resultados de testes já realizados.

## Preparação de controle remoto

A Equipe pode solicitar marcha, limites de velocidade e aceleração e parada
remota ao mestre. O estado é apenas preparatório, sem atuação física. Consulte
[Controle remoto](controle-remoto.md) para o contrato UART/HTTP, intertravamentos,
reinício, acompanhamento no painel e requisitos da integração futura.
