# Protocolos compartilhados

[Índice da documentação](README.md)

Os cabeçalhos em `firmware/compartilhado/protocolos` definem as estruturas
binárias usadas pelo Mestre, pela Equipe e por Visitantes.

## Enlace físico

As duas UARTs do mestre são independentes e bidirecionais. Usam nível de 3,3 V,
8N1 e 1.000.000 baud de `configuracao_enlace_uart.h`. O parser de
`protocolo_enlace_uart.h` distingue telemetria (`AA 55` para dados, `AA 56`
para ACK), comandos (`C7 7C`) e OTA (`A5 5A`) pelo início do quadro. Os inteiros
multibyte dos quadros compactados usam little-endian, ordem nativa do ESP32-S3 e do ESP32-C3.

| Família | Sentido usual | Integridade | Uso |
|---|---|---|---|
| Telemetria v5 | Mestre → satélites | CRC-16/CCITT-FALSE | Retrato periódico do veículo e detalhes do INA228. |
| ACK de telemetria | Satélites → mestre | CRC-16/CCITT-FALSE | Sequência, versão e saúde do enlace. |
| Comandos v1 | Bidirecional | CRC-32 | Consultas, ações e respostas. |
| OTA de satélite v1 | Bidirecional | CRC-32 e SHA-256 final | Início, blocos, finalização e cancelamento. |

## Telemetria e ACK

[`protocolo_telemetria.h`](../firmware/compartilhado/protocolos/protocolo_telemetria.h)
define o quadro v5 de 122 bytes, com carga de 115 bytes e CRC-16/CCITT-FALSE.
Os três firmwares devem usar a mesma
revisão deste arquivo.
Além dos valores brutos, a versão 5 transporta aceleração em g, giroscópio em
graus por segundo, aceleração longitudinal filtrada, temperatura e o estado de
calibração/eixo do MPU6050.
Qualquer alteração no formato deve ser aplicada primeiro neste arquivo.

O pacote começa com marcador, versão, tipo, tamanho, sequência e tempo do
mestre em milissegundos. Depois vêm ADC bruto, aceleração, giroscópio, marcha,
velocidade, pedais, bateria, localização, oito distâncias, potência, autonomia,
percurso, odômetro e estado do MPU. O bit 7 de `marcha` marca origem simulada;
os bits 0 a 6 carregam a marcha. As escalas fazem parte dos nomes dos campos:
`*_centesimos_kmh`, `*_milesimos_ms2`, `*_decimos_percentual`, `*_mv`,
`*_centesimos_a`, `*_micrograus`, `*_cm`, `*_milesimos_g` e
`*_centesimos_graus_s`. A estrutura `pacote_telemetria_t` é a referência para
ordem e tipos. O bit 7 de `estado_mpu`, antes livre, indica que tensão do
barramento, corrente e potência contêm uma medição elétrica válida do INA228
ou dados simulados; os outros bits continuam descrevendo o MPU6050. Sem esse
bit, os três campos elétricos não são leitura
física, mesmo que estejam zerados. No modo real, o SoC corresponde à última
estimativa em repouso. A autonomia é disponibilizada na simulação.

A versão 5 acrescenta queda de tensão no shunt (µV), temperatura do chip
(centésimos de °C), energia acumulada (mWh), carga acumulada com sinal (mAh),
diagnóstico bruto e `estado_ina`. Neste último, bit 0 marca INA presente, bit 1
marca energia válida e bit 2 marca carga acumulada válida. O bit 3 marca a
presença da estimativa de SoC em `carga_decimos_percentual`. O Mestre só envia
esse bit aos satélites que anunciam suporte na confirmação de telemetria.
Os acumuladores começam no
reset do sensor e podem ficar indisponíveis por transbordamento. Esses campos
são zerados quando a leitura física falha ou a aquisição é pausada; em
simulação, só os campos elétricos gerais são simulados.

Atualize os dois satélites junto com o mestre ao instalar esta versão: firmwares
anteriores não interpretam o novo tamanho/versão do quadro. Uma mistura de
versões interrompe a recepção de telemetria.

Cada satélite responde a um quadro válido com uma confirmação de 20 bytes. Ela
repete a sequência e o tempo do mestre para permitir a medição do tempo de ida
e volta sem sincronização de relógios, usando o mesmo CRC-16. A resposta também
informa tipo do satélite e versão do firmware. Um campo interno permite ao mestre
selecionar recursos compatíveis para a OTA; ele não é uma informação do painel.
O receptor classifica sequência contínua, perda, duplicata ou fora de ordem;
tempo do mestre retrocedendo indica reinício e resincroniza o histórico.

## Comandos

[`protocolo_comandos.h`](../firmware/compartilhado/protocolos/protocolo_comandos.h)
define o transporte bidirecional de comandos. O quadro é
genérico: contém código, identificador da solicitação, resultado e uma carga
binária variável protegida por CRC-32. Assim, novos comandos e respostas podem
ser incluídos sem criar outro enquadramento UART. Respostas evolutivas devem
indicar sua versão na própria carga ou continuar reconhecíveis pelo tamanho.

O cabeçalho de 16 bytes carrega marcador, versão, tipo solicitação/resposta,
origem/destino, código, identificador, resultado e tamanho da carga. O CRC-32
ocupa 4 bytes. Com carga de até 128 bytes, o quadro ocupa até 148 bytes.
Resultados possíveis incluem OK, não suportado, carga inválida, ocupado,
negado, erro interno e timeout. O destinatário valida cabeçalho, tamanho e CRC
antes de interpretar a carga. O destinatário responde quando uma operação não
é suportada.

### Compatibilidade anunciada

O protocolo v1 ainda transporta máscaras `capacidades` na resposta de versão,
na consulta específica de capacidades e na confirmação do satélite. A Equipe
usa a máscara do mestre para habilitar ações e decidir quais estados consultar.
O mestre usa bits da confirmação de cada satélite para verificar OTA por UART
e escolher blocos de 4096 ou 2048 bytes. Portanto, esses campos são parte do
contrato **em uso**, mesmo que não sejam informação útil para quem lê o painel.
Uma resposta `NAO_SUPORTADO` continua sendo a confirmação explícita de que um
comando não existe no firmware consultado.

Uma eventual retirada das máscaras exige uma nova versão do protocolo de
comandos/ACK, atualização coordenada dos três firmwares e uma regra explícita
para o tamanho de bloco OTA em pares de versões mistas. Até essa migração, os
campos permanecem internos e não devem aparecer como métricas na interface.

A resposta detalhada de estado OTA usa o formato 2 e informa separadamente o
mestre, o satélite da equipe e o satélite de visitantes. A estrutura antiga foi
mantida para permitir a atualização gradual dos dispositivos.

A página da Equipe envia as ações operacionais no comando UART 31. O campo
`acao` identifica a operação e os campos tipados carregam índices, intervalos
ou credenciais. O menu serial do Mestre chama os serviços locais. Consultas
de configuração e de redes salvas têm códigos próprios. Quando a resposta à
solicitação ainda está memorizada, uma retransmissão recebe a resposta em
cache. O Mestre mantém a última resposta por origem.

`ACAO_MESTRE_DEFINIR_REDE_WIFI_SALVA_COMO_PRINCIPAL` (ação 21) recebe o índice
da rede secundária e a permissão administrativa. O mestre testa a conexão,
grava o perfil como principal e retira essa rede da lista de secundárias.
`resposta_configuracao_mestre_t` inclui o SSID principal e se ele foi alterado
em relação ao padrão do firmware, permitindo a confirmação posterior na página.

`COMANDO_OBTER_ESTADO_SIMULADOR` (código 37) consulta o simulador e
`ACAO_MESTRE_DEFINIR_SIMULADOR` (ação 16, valor 0 ou 1) altera sua fonte.
A resposta informa estado atual, padrão compilado, dinâmica e total de amostras geradas. A mudança
é volátil: uma reinicialização restaura `USAR_TELEMETRIA_SIMULADA`.

## OTA dos satélites

[`protocolo_ota_satelites.h`](../firmware/compartilhado/protocolos/protocolo_ota_satelites.h)
define um cabeçalho de 18 bytes com sessão, sequência, deslocamento, tipo e
tamanho da carga, e um CRC-32 de 4 bytes. Os tipos são iniciar, dados, finalizar,
cancelar e resposta. O limite atual é 4096 bytes de carga por bloco, para um
quadro de até 4118 bytes. O tamanho de 2048 bytes permanece
como tamanho compatível para versões antigas. O início leva tamanho total,
SHA-256 esperado e versão. Cada bloco recebe confirmação; o satélite rejeita
sessão, sequência, deslocamento e tamanho incompatíveis. Um cancelamento de
sessão antiga não aborta uma transferência nova; a resposta informa sessão
inválida. Os receptores verificam o prazo da OTA também após cada lote UART,
sem depender de uma pausa no tráfego. Ao finalizar, compara o hash completo
antes de selecionar a partição inativa e reiniciar. O
[guia de OTA](ota.md) cobre publicação,
autorização e rollback.

## Modelo da aplicação e testes

[O modelo interno do Mestre](../firmware/mestre-s3/include/nucleo/dados_telemetria_veiculo.h)
reúne medições em unidades como km/h e m/s², contagens brutas e indicadores de
validade antes da compactação. O serviço do Mestre
converte esse modelo para `pacote_telemetria_t`; os satélites usam o mesmo
cabeçalho binário para reconstruir as amostras. Os contratos e as rotinas
portáveis de codificação e validação ficam em
`firmware/compartilhado/protocolos`.

A suíte em `firmware/mestre-s3/test/test_protocolo_telemetria` verifica tamanho
e serialização, CRC-16, limites de conteúdo, ACK, sequência com perda,
duplicação e retorno, além da recuperação do parser após quadro incompleto ou
corrompido. Ela não é incluída no firmware normal e pode ser executada com o
executor de testes do PlatformIO no ambiente do mestre.

## Controle de marcha e acelerador

A Equipe usa ações do comando UART 31 para limite de faixa L/M/H, permissão de
ré, limites de velocidade e pedal, parada e rearme. O acelerador físico fornece
a leitura de pedal ao Mestre.

A Equipe solicita o formato 7 no comando de consulta UART 41. A resposta de
66 bytes acrescenta `reversao_permitida` e `rearme_restante_ms` aos 61 bytes do
formato 6. A Equipe também lê o formato 6 de um Mestre antigo. O
enquadramento de comandos continua v1, e a referência dos campos e unidades
é [`controle_remoto.h`](../firmware/compartilhado/protocolos/controle_remoto.h).

O valor 5 em `estado_operacao` identifica `CONTROLE_ESTADO_LIMITANDO_VELOCIDADE`.
Durante a limitação progressiva normal, o Mestre reduz a referência do pedal
preservando a marcha. O bit `CONTROLE_BLOQUEIO_VELOCIDADE` permanece definido
para interpretar respostas de firmwares antigos.

O Mestre verifica o instante da primeira recepção recente de um pedido de
rearme usando seu relógio local. O pedido deve chegar depois dos **5 s** de
espera da parada ativa e ser executado em até **2 s** após a recepção. Assim,
uma espera na fila não torna válido um pedido antigo ou anterior à janela
de rearme da parada atual. A validação acontece no Mestre e mantém os mesmos
campos UART. Consulte as condições adicionais em
[Controle de marcha e acelerador](controle-remoto.md).
