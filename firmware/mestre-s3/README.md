# Firmware do ESP32-S3 mestre

Alvo de hardware: **ESP32-S3-WROOM-1-N16R8**, com 16 MB de flash Quad SPI e
8 MB de PSRAM Octal SPI.

Adquire tensão e MPU6050 ou gera uma telemetria simulada, monta o pacote e o envia
aos módulos da equipe e de visitantes por UART. O modo é escolhido por
`USAR_TELEMETRIA_SIMULADA` em `configuracao_placa.h`; enquanto estiver habilitado,
o terminal identifica claramente os dados como simulados. Também verifica
atualizações no GitHub e só executa
o OTA após autorização pelo terminal. Consulte `ARQUITETURA.md` para a divisão
interna e `../../documentacao/ota/README.md` para preparar os releases.

## Redes Wi-Fi pelo terminal

No menu serial, a opção `5` permite testar uma rede informada pelo usuário, decidir
se ela deve ser salva, conectar a redes já salvas e excluí-las. A rede principal é
sempre tentada primeiro na inicialização. Redes secundárias só são tentadas após
confirmação, salvo quando o modo automático tiver sido habilitado por uma operação
protegida.

A troca da rede principal, sua restauração e a alteração do modo automático exigem
`WIFI_SENHA_PERMISSAO` em `include/nucleo/segredos_ota.h`. Se a constante não for
definida, o firmware usa `OTA_WIFI_SENHA` por compatibilidade. As credenciais e as
preferências ficam na NVS; nenhuma senha aparece nos relatórios ou nos logs.

Canal preferencial, largura e potência do rádio são configurados em
`include/nucleo/configuracao_wifi.h` pelas flags `WIFI_CANAL_PREFERENCIAL`,
`WIFI_LARGURA_CANAL_MHZ` e `WIFI_POTENCIA_MAXIMA_DBM`. Como o mestre trabalha
em modo estação, o canal é apenas uma indicação de onde iniciar a busca; o
roteador determina o canal usado após a associação. O valor `0` aceita qualquer
canal.

As funções públicas de `include/servicos/servico_wifi.h` não dependem do menu. Elas
podem ser chamadas futuramente pelo serviço de telemetria mantendo as mesmas
validações e regras de persistência.

## LEDs e buzzer passivo

O mestre possui um serviço não bloqueante de sinalização para inicialização,
saúde, Wi-Fi, OTA, avisos, falhas e confirmações do terminal. Os quatro LEDs usam
os GPIOs 10 a 13 e o buzzer passivo usa PWM no GPIO 14. As ligações e o circuito
de acionamento recomendado estão em `../../hardware/pinagem/README.md`.

O acesso elétrico fica em `gerenciador_indicadores`; a interpretação dos estados
e as melodias ficam em `servico_sinalizacao`. Assim, os demais serviços apenas
publicam seu estado ou um evento e não precisam conhecer GPIO, PWM ou notas.

| Indicação | Significado |
|---|---|
| Azul e amarelo em animação | Inicialização |
| Verde contínuo | Sistema saudável |
| Verde com pulso azul duplo | Sessão Wi-Fi ativa |
| Azul e amarelo alternados | Verificação OTA |
| Azul contínuo com pulso amarelo duplo | Atualização aguardando autorização |
| Azul com animação verde e amarela | Firmware OTA sendo baixado |
| Sequência verde, azul e amarela | Atualização concluída |
| Amarelo duplo | Atenção na saúde do sistema |
| Amarelo contínuo com vermelho duplo | Falha OTA; alarme a cada 15 segundos |
| Vermelho em SOS | Falha crítica; alarme a cada 8 segundos |

## Encoder e odômetro

No modo de sensores reais, duas unidades do periférico PCNT leem os encoders das
rodas esquerda (GPIOs 5/6) e direita (GPIOs 7/21). Todas as transições são acumuladas em hardware, portanto
deslocamentos para frente e para trás sempre aumentam o percurso sem polling da
CPU. Cada dispositivo converte sua configuração de 600 P/R em 2400 contagens por
giro. O serviço aplica o diâmetro e a relação `giros do encoder / giro da roda`
de cada lado e usa a média das duas distâncias para velocidade, percurso e
odômetro. Os parâmetros ficam em
`include/nucleo/configuracao_placa.h`.

`percurso_km` começa em zero a cada inicialização. `odometro_km` é recuperado da
NVS; a constante `INTERVALO_PERSISTENCIA_ODOMETRO_M` define a distância entre
gravações e começa em 100 m. A configuração atual usa os sensores reais
(`USAR_TELEMETRIA_SIMULADA=false`). O menu serial oferece a opção **Sensores e
diagnóstico** para consultar ADC, MPU6050, PCNT, percurso, odômetro, NVS e erros.

As constantes `VELOCIDADE_MAXIMA_ODOMETRO_KMH`,
`RAZAO_MINIMA_MOVIMENTO_ENTRE_RODAS`,
`MOVIMENTO_MINIMO_DIAGNOSTICO_RODAS_M` e
`AMOSTRAS_DIVERGENTES_PARA_ALERTA` rejeitam saltos fisicamente impossíveis e
avisam quando um encoder permanece muito abaixo do outro. Elas não mudam a
regra de cálculo do odômetro pela média. Uma falha de gravação fica pendente e é
tentada novamente com intervalo, evitando repetir `commit` a cada ciclo.

## Tela local SSD1306

Uma tela SSD1306 I²C de 0,96 polegada e 128×64 pixels pode mostrar o estado do
mestre sem depender do terminal. Ela apresenta inicialização, saúde dos satélites,
sessão Wi-Fi, etapas da atualização OTA, avisos e falhas críticas. O painel usa os
mesmos GPIOs 8 e 9 do MPU6050 e detecta automaticamente os endereços `0x3C` e
`0x3D`.

Durante a partida, a tela identifica o módulo em inicialização e apresenta uma
barra de progresso. Avisos gerados nessa fase entram em uma fila fixa e são
mostrados em sequência quando o sistema começa a operar. Mensagens persistentes,
como atualização disponível ou atenção de diagnóstico, ocupam a tela por uma
janela curta e depois reaparecem como lembretes; entre essas janelas, as páginas
de telemetria e estado continuam visíveis.

O framebuffer fica em `dispositivos/tela_ssd1306.cpp`. A seleção das mensagens e
a atualização periódica ficam em `servicos/servico_painel_local.cpp`, sem misturar
o protocolo elétrico da tela com as regras de estado da aplicação.

Eventos de partida, confirmação, Wi-Fi, atualização disponível, transferência,
conclusão, aviso e falha possuem melodias próprias e não bloqueantes. A conclusão
de uma OTA é numerada pelo serviço OTA, permitindo que tela e buzzer percebam
também a atualização de um satélite mesmo quando o estado geral muda rapidamente.

## Histórico de telemetria no Google

Os satélites recebem o estado ao vivo a 4 Hz, enquanto o mestre registra no
cartão SD uma amostra por segundo. Os CSVs cobrem até uma hora ou 2 MiB e o
upload só começa quando a opção correspondente é escolhida no terminal. O envio
é feito em blocos e a tela local mostra arquivo atual, progresso, velocidade,
estimativa e quantidade restante, seguida de um resumo. Uma falha de rede
preserva os arquivos pendentes e permite repetir a operação sem duplicar linhas.
A pinagem, o Web App e a configuração das credenciais estão em
`../../documentacao/telemetria-google/README.md`.

## Diagnóstico do ESP e tarefas

A política de núcleos, prioridades e pilhas fica em
`include/nucleo/configuracao_tarefas.h`. Os relatórios incluem uso de CPU por
tarefa, afinidade e menor margem de pilha. A supervisão executa recuperações
fora da tarefa de prioridade alta e acompanha fases de aquisição, SD e rede.
A opção 1 apresenta o resumo por seções e as tarefas em tabela, ordenadas pela
menor margem, além de indicar qual estado dos LEDs foi comandado.

No cartão, `logs/esp_<sessao>_<parte>.log` reúne identificação do firmware,
motivo do reset, memória interna/PSRAM/DMA, tarefas e erros. Há registros a cada
30 segundos e em mudanças relevantes, com segmentos de aproximadamente 1 MiB
e limite aproximado de 8 MiB por sessão. Ao atingir esse limite, a telemetria
continua sendo gravada. Esses logs não são enviados ao Google nem apagados
automaticamente. Veja limites e interpretação em `ARQUITETURA.md`.

Validação em bancada: exercitar aquisição junto com upload e depois OTA,
interromper a rede, verificar os alertas e conferir os logs após reiniciar.
Comparar memória interna, maior bloco e margens mínimas de pilha durante TLS;
a compilação não substitui esse teste com o cartão e os periféricos reais.
