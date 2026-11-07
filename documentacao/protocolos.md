# Protocolos compartilhados

[Índice da documentação](README.md)

Esta pasta contém estruturas binárias e regras que precisam ser idênticas no mestre,
no módulo da equipe e no módulo de visitantes.

## Enlace físico

As duas UARTs do mestre são independentes e bidirecionais. Usam nível de 3,3 V,
8N1 e 1.000.000 baud de `configuracao_enlace_uart.h`. O parser de
`protocolo_enlace_uart.h` distingue telemetria (`AA 55` para dados, `AA 56`
para ACK), comandos (`C7 7C`) e OTA (`A5 5A`) pelo início do quadro. Os inteiros
multibyte dos quadros compactados usam little-endian, ordem nativa dos ESP32-S3.

| Família | Sentido usual | Integridade | Uso |
|---|---|---|---|
| Telemetria v4 | Mestre → satélites | CRC-16/CCITT-FALSE | Retrato periódico do veículo. |
| ACK de telemetria | Satélites → mestre | CRC-16/CCITT-FALSE | Sequência, versão e saúde do enlace. |
| Comandos v1 | Bidirecional | CRC-32 | Consultas, ações e respostas. |
| OTA de satélite v1 | Bidirecional | CRC-32 e SHA-256 final | Início, blocos, finalização e cancelamento. |

## Telemetria e ACK

`protocolo_telemetria.h` define o quadro v4 de 105 bytes, com versão, tipo,
tamanho da carga e CRC-16/CCITT-FALSE. Os três firmwares devem usar a mesma
revisão deste arquivo.
Além dos valores brutos, a versão 4 transporta aceleração em g, giroscópio em
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
ordem e tipos. Um campo no protocolo pode não ter sensor no modo real atual;
zero não comprova medição física.

Cada satélite responde a um quadro válido com uma confirmação de 20 bytes. Ela
repete a sequência e o tempo do mestre para permitir a medição do tempo de ida
e volta sem sincronização de relógios, usando o mesmo CRC-16. A resposta também
informa tipo do satélite e versão do firmware. Um campo interno permite ao mestre
selecionar recursos compatíveis para a OTA; ele não é uma informação do painel.
O receptor classifica sequência contínua, perda, duplicata ou fora de ordem;
tempo do mestre retrocedendo indica reinício e resincroniza o histórico.

## Comandos

`protocolo_comandos.h` define o transporte bidirecional de comandos. O quadro é
genérico: contém código, identificador da solicitação, resultado e uma carga
binária variável protegida por CRC-32. Assim, novos comandos e respostas podem
ser incluídos sem criar outro enquadramento UART. Respostas evolutivas devem
indicar sua versão na própria carga ou continuar reconhecíveis pelo tamanho.

O cabeçalho carrega marcador, versão, tipo solicitação/resposta, origem/destino,
código, identificador, resultado e tamanho. A carga tem no máximo 128 bytes.
Resultados possíveis incluem OK, não suportado, carga inválida, ocupado,
negado, erro interno e timeout. O destinatário valida cabeçalho, tamanho e CRC
antes de interpretar a carga. O destinatário responde quando uma operação não
é suportada.

A resposta detalhada de estado OTA usa o formato 2 e informa separadamente o
mestre, o satélite da equipe e o satélite de visitantes. A estrutura antiga foi
mantida para permitir a atualização gradual dos dispositivos.

As operações do menu serial do mestre usam um único comando operacional
estruturado. O campo `acao` identifica a operação e os campos tipados carregam
índices, intervalos ou credenciais; isso evita criar um código de protocolo para
cada botão da interface. Consultas de configuração e de redes salvas permanecem
separadas por não terem efeito colateral. A repetição de uma solicitação devolve
a resposta memorizada e não executa a ação novamente.

`COMANDO_OBTER_ESTADO_SIMULADOR` (código 37) consulta o simulador e
`ACAO_MESTRE_DEFINIR_SIMULADOR` (ação 16, valor 0 ou 1) altera sua fonte.
A resposta informa estado atual, padrão compilado, dinâmica e total de amostras geradas. A mudança
é volátil: uma reinicialização restaura `USAR_TELEMETRIA_SIMULADA`.

## OTA dos satélites

`protocolo_ota_satelites.h` usa sessão, sequência, deslocamento, tipo e tamanho
da carga no cabeçalho. Os tipos são iniciar, dados, finalizar, cancelar e
resposta. O limite atual é 4096 bytes de carga por bloco; 2048 bytes permanece
como tamanho compatível para versões antigas. O início leva tamanho total,
SHA-256 esperado e versão. Cada bloco recebe confirmação; o satélite rejeita
sessão, sequência, deslocamento e tamanho incompatíveis. Ao finalizar, compara
o hash completo antes de selecionar a partição inativa e reiniciar. O
[guia de OTA](ota.md) cobre publicação,
autorização e rollback.

## Modelo da aplicação e testes

`firmware/compartilhado/telemetria/dados_telemetria_veiculo.h` descreve o
retrato em unidades de engenharia antes da compactação. O serviço do mestre
converte esse modelo para `pacote_telemetria_t`; os satélites usam o mesmo
cabeçalho binário para reconstruir as amostras. Utilitários pequenos usados por
mais de um firmware ficam em `firmware/compartilhado/comum`.

A suíte em `firmware/mestre-s3/test/test_protocolo_telemetria` verifica tamanho
e serialização, CRC-16, limites de conteúdo, ACK, sequência com perda,
duplicação e retorno, além da recuperação do parser após quadro incompleto ou
corrompido. Ela não é incluída no firmware normal e pode ser executada com o
executor de testes do PlatformIO no ambiente do mestre.

## Preparação de controle remoto

A Equipe pode solicitar marcha, limites de velocidade e aceleração e parada
remota ao mestre. O estado é apenas preparatório, sem atuação física. Consulte
[Controle remoto](controle-remoto.md) para o contrato UART/HTTP, intertravamentos,
reinício, acompanhamento no painel e requisitos da integração futura.
