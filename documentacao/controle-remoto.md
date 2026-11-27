# Controle de marcha, acelerador e parada

## Partida, atualização e acionamento

O Mestre inicia desarmado. O operador usa **Armar** após a inicialização e
pelo menos 5 s desde a partida ou a última parada. O Mestre verifica a pinagem
habilitada, a marcha física, os sensores reais e coerentes, velocidade de até
0,5 km/h, limites positivos, pedal solto por 500 ms e escrita de 0 V no DAC
confirmada pelo I²C. A marcha respeita o teto configurado e o sentido inicial
é frente. O armamento também exige uma nova amostra de pedal em zero antes
de liberar a tração.

Uma instalação OTA de qualquer alvo ativa a parada antes da transferência e
impede o rearme durante ela. Quando a instalação termina na mesma sessão do
Mestre, a parada permanece ativa até o operador rearmar. Se o Mestre reiniciar,
volta à sequência de partida descrita acima. Consultar atualizações preserva
o estado de controle.

O relé previsto usa o contato **normalmente aberto (NA)**: bobina desligada
significa entrada de habilitação do driver aberta. O código prepara as saídas
de marcha L (modo A), M (nenhum modo), H (modo B), ré e bobina do relé,
desligando o relé antes de mudar os demais sinais e aguardando 100 ms para
religá-lo. O GPIO21 fecha o contato quando o Mestre autoriza a tração e confirma zero no DAC.
Um resistor físico na base do transistor deve garantir bobina desligada
durante reset e antes do firmware iniciar. Sem medir o circuito real, o estado
de software não confirma que o driver foi desabilitado.

Os botões **Salvar velocidade** e **Salvar acelerador** da página Equipe
gravam os limites na NVS do mestre. Eles são restaurados após reinício; o
armamento depende das verificações acima.

[Índice](README.md) · [Protocolos](protocolos.md) · [Painéis](satelites.md)

## O que funciona agora

A página **Controle** da Equipe envia ao mestre a marcha máxima (`L`, `M`, `H`),
a permissão de ré, os limites de velocidade e acelerador, a zona morta do pedal
e a parada remota. O mestre lê o pedal Hall no **GPIO5**
pela unidade ADC1, com calibração do ADC; a faixa informada para esse pedal é
**0,5–2,5 V**. A tarefa de controle converte o curso útil em 0–100%, aplica a
zona morta e o limite do acelerador e calcula a referência para o MCP4725.
Com a parada ativa, o operador pode ajustar marcha máxima, permissão de ré,
velocidade máxima, acelerador máximo e zona morta com a saída em zero.
O DAC é procurado no endereço I²C `0x60` e recebe comando de **0 V** ao
iniciar. A atuação física segue a [pinagem do Mestre](pinagem.md#interface-de-controle-da-tração). A chave física escolhe a marcha e a direção; o painel configura a marcha máxima e a permissão de ré.

Ao receber a parada remota, o Mestre zera a referência lógica do acelerador
e abre o relé configurado pelo GPIO. A tarefa de controle comanda 0 V no DAC.
A chegada do pedido depende do navegador, do
Wi-Fi e da UART. O estado exibido confirma o processamento no Mestre. A
desaceleração e a desabilitação do controlador exigem verificação física. A
página de Visitantes oferece somente leitura. Os comandos operam sobre o
estado do Mestre, inclusive quando a simulação bloqueia a tração.

## Operação pelo painel

Na página **Controle**, velocidade, corrente, tensão e potência da telemetria
aparecem perto dos comandos. O acelerador real é o percentual da última
referência confirmada no DAC, após limites e rampa. O freio e o estado de carga
vêm da simulação quando ela está ativa. A leitura bruta do pedal Hall aparece
na consulta de calibração e estado do pedal. O resumo de controle mostra
marcha, sentido, limites e o tempo restante da parada. A
resposta da consulta também traz os bits de bloqueio para diagnóstico. A
aceitação de um comando e a consulta posterior são apresentadas separadamente. A página
**Comandos** mantém o histórico de ações; o estado de Controle também é
consultado automaticamente, além do botão **Atualizar estado**.

1. Defina a faixa máxima, um limite positivo de velocidade e um teto do
   acelerador entre 0 e 100%. Qualquer um dos dois limites em zero bloqueia a
   tração. O limitador usa a velocidade das rodas.
2. Aguarde pelo menos 5 s desde a última ativação da parada, solte o pedal e
   use **Armar**. O painel libera a confirmação após 3 s. A partida e a OTA
   também exigem armamento manual.
3. Com o pedal em zero e velocidade medida de até 0,5 km/h, selecione a
   faixa pela chave física. O painel regula a faixa máxima. `L` solicita
   modo A, `M` deixa A/B abertos e `H` solicita modo B. A atuação desses
   sinais depende da habilitação elétrica da placa.
4. A ré vem permitida por padrão. Use **Permitir ré** ou **Bloquear ré**
   para alterar essa autorização com confirmação após 3 s. Na
   mudança de sentido, os intertravamentos exigem pedal em zero e velocidade
   medida de até 0,5 km/h. O sentido é independente de `L/M/H`.
5. A zona morta fica em RAM até novo ajuste ou reinício. O teto percentual é
   salvo na NVS. A tensão mínima padrão é `DAC_TENSAO_MINIMA_SAIDA_MV` em
   `include/nucleo/configuracao/veiculo.h`. Faça os ajustes de calibração com
   a parada ativa ou com o pedal solto. A falta de leitura local recente mantém
   a referência em zero.

**Parada de emergência** não pede confirmação e é enviada sem aguardar a fila de
consultas do navegador. O mestre zera o acelerador no recebimento do comando,
retira a marcha e o sentido solicitado e marca corte por relé. Após a consulta
confirmar o estado, a página exibe um aviso flutuante próprio para a parada,
sem afirmar que houve atuação física. A parada é
encerrada pelo botão **Armar** após o prazo de 5 s e com os intertravamentos
atendidos. Cada ativação reinicia esse prazo. Após o armamento, uma
nova amostra do pedal em zero é necessária antes de liberar a tração.

Sem resposta, o valor anterior na tela não confirma o novo comando. Dois
navegadores compartilham o estado do mestre: a revisão ajuda a perceber
alterações feitas por outro operador, mas não oferece exclusividade. A Equipe
reserva uma vaga HTTP para ações, separada das vagas de consulta; mesmo assim,
transporte e processamento podem atrasar ou falhar.

## Estado, limites e reinício

| Campo | Unidade/faixa | Inicial |
|---|---|---|
| Faixa solicitada | 0 nenhuma; 1 `L`; 2 `M`; 3 `H` | Nenhuma |
| Faixa máxima | `L`, `M` ou `H` | `L` |
| Sentido solicitado | 0 frente; 1 ré | Frente |
| Permissão de ré | 0 bloqueada; 1 permitida | Permitida |
| Reversão autorizada pelo estado | 0 desligada; 1 ré solicitada e sem bloqueio | Desligada |
| Velocidade máxima | 0–35 km/h, resolução 0,01 | 0 |
| Teto do acelerador | 0–100%, resolução 0,1% | 0 |
| Zona morta do pedal | 0–30%, resolução 0,1% | 5% |
| Tensão mínima da saída DAC | 0–2,5 V, resolução 0,001 V | 1 V |
| Pedal lido/acelerador permitido | 0–100%, resolução 0,1% | 0 |
| Parada | Ativa ou encerrada por rearme | Ativa |
| Saída do DAC habilitada | Falsa até validação elétrica | Falsa |

Reduzir a faixa máxima abaixo da selecionada retira a seleção e zera o
acelerador lido e permitido. Alterar o limite de velocidade também zera essa
referência. Um limite de velocidade ou teto do acelerador igual a zero bloqueia
a tração. Durante operação normal, o limitador reduz a referência antes do
limite e permite retomada em rampa após confirmar a queda da velocidade.

`reversao_solicitada` registra a escolha frente/ré do operador.
`reversao_acionar_solicitado` só acompanha essa escolha quando o estado está
pronto; é zero durante parada ou qualquer bloqueio. Esse segundo campo também
é apenas um resultado do controle em software, sem comando elétrico no pino.

O campo `estado_operacao` distingue **parado** (parada ativa), **bloqueado**
(intertravamento), **pronto**, **acelerando** e **limitando velocidade**.
`bloqueios` é uma máscara cujos bits podem aparecer juntos e ajudam a
explicar por que o valor permitido é zero. A definição numérica fica no
[cabeçalho compartilhado](../firmware/compartilhado/protocolos/controle_remoto.h).

| Bloqueio | Condição |
|---|---|
| Sem faixa | Nenhuma das faixas L/M/H foi escolhida. |
| Sem limites | Velocidade máxima ou teto do acelerador igual a zero. |
| Sem sensores | MPU ou odômetro sem leitura real, recente e coerente. |
| Velocidade | Rodas acima do limite de velocidade para armar. |
| Telemetria simulada | A fonte simulada está ativa. |
| Pedal indisponível | Amostra local ausente, vencida ou aguardando zero. |
| Falha travada | Falha de saída travada ou ciclo em que a perda de sensor com pedal ativo solicitou a parada. |
| Partida ou OTA | Inicialização incompleta ou instalação OTA em andamento. |

O estado fica em RAM. Reiniciar o mestre restaura parada ativa, faixa nenhuma,
frente, ré permitida, faixa máxima `L`, zona morta de 5%, tensão mínima de
1 V e pedal em zero. Os limites de velocidade e acelerador são restaurados da
NVS; na falta de valor salvo, começam em zero. O mestre tenta zerar o
MCP4725; nenhum GPIO de marcha, reversão ou relé é comandado.

## Contrato HTTP e UART

Na aba **Controle**, **Permitir ré** libera a seleção de ré. **Bloquear ré**
revoga a permissão e aciona a parada se já houver uma ré solicitada. O Mestre
inicia com a ré bloqueada e valida a permissão antes de autorizar a saída de
reversão. O painel habilita **Confirmar** após 3 s para mudanças de permissão
e para rearme manual. A parada de emergência é enviada ao pressionar.
Na configuração atual, o pino de reversão está sem ligação. A restrição vale
para os comandos que passam pelo Mestre.

Todos os POST usam `/api/mestre?consulta=NOME`, com corpo textual contendo
somente um inteiro decimal sem sinal. O servidor HTTP rejeita caracteres extras;
o mestre valida novamente a faixa e as condições de estado.

| Nome HTTP | Ação UART dentro de `COMANDO_EXECUTAR_ACAO_MESTRE` (31) | Campo `valor` |
|---|---|---|
| `limite_marcha` | 23 | 1 `L`, 2 `M`, 3 `H` |
| `definir_reversao` | 25 | 0 frente, 1 ré |
| `permitir_re` | 31 | 0 bloquear ré, 1 permitir ré |
| `velocidade_maxima` | 18 | Centésimos de km/h, 0–3500 |
| `limite_acelerador` | 24 | Teto em décimos de percentual, 0–1000 |
| `zona_morta_pedal` | 26 | Zona morta em décimos de percentual, 0–300 |
| `tensao_minima_dac` | 27 | Saída mínima em milivolts, 0–2500 |
| `parada_remota` | 20 | 1 ativar parada; 0 rearmar controle |

O botão **Armar** envia `parada_remota` com valor `0`.
O Mestre só aceita o rearme manual após pelo menos 5 s desde a última parada,
com o pedal solto por 500 ms, os sensores válidos e velocidade de até 0,5 km/h.
A marcha vem da chave física e respeita o limite remoto. O Mestre usa o
instante local da primeira chegada pela UART enquanto mantém o identificador
do pedido no cache. Uma retransmissão com o mesmo identificador preserva esse
instante. O pedido deve chegar após os 5 s da parada atual e ser processado em
até 2 s. Uma nova parada invalida pedidos anteriores ainda na fila. Os demais
campos da solicitação estruturada devem ser zero. A resposta de sucesso
registra a aceitação do valor pelo Mestre.
Somente a origem Equipe pode executar essas ações ou consultar o estado. O
cache de solicitações evita reexecução de retransmissões com o mesmo ID.

`GET /api/mestre?consulta=controle_remoto` usa o comando UART 41. A Equipe
solicita o **formato 7**, que acrescenta `reversao_permitida` e
`rearme_restante_ms`, e lê o formato 6 quando o Mestre é antigo. A resposta diferencia
marcha física, marcha permitida e máxima, sentido escolhido e reversão autorizada pelo estado,
parada, corte por relé, modo A/B, teto, pedal lido e acelerador permitido,
velocidade e aceleração medidas, bloqueios, tensão do pedal, referência do DAC,
disponibilidade dos dispositivos e prazos restantes. A ordem e as
unidades de todos os campos estão em
`firmware/compartilhado/protocolos/controle_remoto.h`. O servidor da Equipe
converte a estrutura para JSON; `revisao` e `alterado_em_ms` ajudam a distinguir
estado antigo de uma nova alteração e podem dar a volta após uso prolongado.

O quadro de comandos permanece v1. O bit 13 indica suporte ao controle remoto
e o bit 18 anuncia a permissão de ré. Atualize Mestre e Equipe para usar essa
permissão no painel. Visitantes mantém as consultas públicas existentes.

## Supervisão e prioridade

`servico_controle_remoto` mantém o estado em RAM e executa uma tarefa no núcleo
1 com ciclo nominal de **20 ms** e prioridade **9**, acima das demais tarefas
definidas no projeto. Ativar a parada passa à frente das ações comuns de Wi-Fi,
OTA e Google. O tratamento zera a referência e retira a faixa no estado do
Mestre. A tarefa de controle recalcula os bloqueios e a aplicação das saídas.
O rearme manual entra na fila normal. O ciclo de controle usa amostras locais
da aceleração e do odômetro. Rede, UART e barramento I²C introduzem atrasos, e
a cadência nominal da tarefa não determina o prazo entre toque e corte físico.

Com a saída do DAC habilitada, o Mestre reenvia a referência ao MCP4725 a
cada 20 ms (50 Hz) por padrão, mesmo com tensão constante. O intervalo é
ajustável em **Comandos → Desenvolvedor**. Com a saída bloqueada, o Mestre
comanda 0 V e reafirma o valor uma vez por segundo.

O odômetro atualiza as rodas a cada **20 ms**, e a telemetria consulta a última
amostra no ritmo do enlace. O INA228 e o MPU6050 são lidos a **10 Hz**. A contagem bruta do ADC vem da leitura do pedal no ciclo de controle. A persistência do odômetro
é processada fora do ciclo das rodas.

O Mestre exige odômetro coerente e velocidade dos encoders de até **150 ms**, MPU
calibrado e leitura inercial de até **300 ms**. O MPU é um intertravamento já
existente. O cálculo do limitador usa apenas a velocidade da roda mais rápida.
Simulação, leitura inválida, parada ativa, faixa ausente ou limite zero mantêm
a saída em zero. Falha dos sensores ou perda de uma leitura válida de pedal com
pedal ativo provoca parada. Partida, parada, rearme e mudança do limite salvo
exigem nova leitura de pedal em zero.

Em operação normal, o menor valor entre pedido do pedal, teto do acelerador e
teto de velocidade define a saída. A faixa progressiva inicial é de até
**3 km/h** antes do limite. Ao chegar ao limite, a referência zera mantendo a
faixa e a habilitação do relé, se os demais intertravamentos estiverem
atendidos. A retomada exige duas amostras novas abaixo da margem de até
**0,30 km/h** e pode ocorrer com o pedal pressionado. Ambas as margens diminuem
para limites baixos. O limitador atua sobre a tração positiva. A velocidade
pode aumentar em descida com a referência do acelerador já em zero. Valide as
margens de [`veiculo.h`](../firmware/mestre-s3/include/nucleo/configuracao/veiculo.h)
em bancada.
Tensão fora da faixa plausível de **0,3–2,7 V**, erro de ADC ou leitura pausada
pelo terminal também bloqueiam o pedal. Se a falha ocorrer com pedal ativo,
o mestre entra em parada. A zona morta retira o início do curso e reescala o
restante até 100%. Com referência permitida zero, o comando ao DAC é zero;
acima disso, a tensão parte do mínimo configurado e sobe até o valor nominal de
5 V segundo o percentual permitido. A rampa limita o aumento da referência a
25 pontos percentuais por segundo. A redução é imediata no cálculo da referência.

O menu 12 **Controle do veículo e pedal** do Mestre consulta a tensão do pedal
e o estado do DAC, ajusta zona morta, tensão mínima e acelerador máximo e oferece
os mesmos comandos de armar e parar. A página Controle oferece os ajustes de zona morta e teto
do acelerador. A tensão do DAC no painel é a **última referência aceita no
I²C**. Meça a saída no pino para validar a tensão real e o tempo de resposta.

`servico_comandos` adapta a UART ao serviço; o servidor da Equipe adapta HTTP
à UART; a página apresenta o resultado. O contrato compartilhado contém apenas
tipos e constantes, evitando dependências circulares. As atualizações e cópias
do estado usam seção crítica curta, sem alocação dinâmica no caminho de
parada. O AP da Equipe aceita ações dos clientes conectados sem identificar um
operador exclusivo; a revisão do estado não substitui autorização e ensaio.

## Integração com o controlador

O GPIO5 lê o pedal e o MCP4725 aplica a referência do acelerador. Os modos
A/B usam GPIO14/13, a ré usa GPIO47 e o relé usa GPIO21. Uma solicitação de ré
sem permissão bloqueia a tração. A troca de direção em movimento ou com o pedal
pressionado desarma o veículo.

Na mesma PCB, o contato comum de K1 recebe `+48V`, o normalmente aberto leva
ao `DRV_ENABLE_SW` e o normalmente fechado fica sem ligação. É necessário
confirmar no circuito real o estado de `DRV_ENABLE_SW` com a bobina energizada,
desenergizada, durante reset e na falta de alimentação. A capacidade do relé
de interromper a corrente dessa entrada na tensão **máxima** da bateria ainda
depende de medição e da especificação de comutação em corrente contínua. A
escolha do contato **normalmente aberto** corresponde à ligação prevista.
Com a bobina sem energia, a entrada de habilitação deve permanecer aberta.
O hardware de parada ainda precisa ser ensaiado antes de qualquer acionamento
pelo mestre, inclusive durante reset e perda da alimentação.

O MCP4725 alimentado em **5 V** pode produzir saída analógica até próximo de
5 V, mas exige nível alto de pelo menos **3,5 V** em SDA/SCL. Use conversor
bidirecional de nível lógico: ESP, MPU6050, INA228 e OLED no lado de **3,3 V**;
MCP4725 no lado de **5 V**, com terra comum e pull-ups adequados em cada lado.
Os pull-ups do lado do ESP e a entrada ADC do pedal respeitam os níveis de 3,3 V.

O MCP4725 restaura ao ligar o valor de sua EEPROM, inclusive antes da
inicialização do mestre. O firmware **não grava a EEPROM** em operação; ao
iniciar, tenta escrever 0 V no registrador volátil. Isso não substitui um
intertravamento físico que mantenha a tração desabilitada durante boot, perda
de alimentação, travamento do ESP ou falha do I²C. Se uma escrita I²C falhar
enquanto o DAC estiver em valor não nulo, o mestre registra a parada lógica,
mas não consegue comprovar que a saída foi a zero. É necessário validar um
corte físico independente antes de habilitar `HABILITAR_SAIDA_ACELERADOR_DAC`.

Os transistores A/B devem ser mutuamente exclusivos. Na mudança de faixa,
retire o modo anterior e confirme em bancada o tempo de acomodação antes de
aplicar o novo. A transição para atuação física exige retorno independente
do relé e do driver, verificação da faixa da entrada de aceleração,
autorização do operador e ensaios sob falha de alimentação ou enlace. O
campo `atuacao_habilitada` indica DAC, chave física e saídas digitais
habilitados após iniciar o serviço. A confirmação elétrica depende de
medição e de um intertravamento independente.

## Prévia e validação

`python ferramentas/visualizar_painel_equipe.py --rede` apresenta a página
com dados e comandos simulados. Use a prévia para conferir a interface e os
estados mostrados. Para verificar o Mestre, registre em bancada os instantes
da chegada do comando, da mudança de referência, da escrita I²C e do corte
no circuito isolado. Valide a perda de cada encoder e dos dois, a volta do sinal,
o limitador com pedal constante, o reboot e a disputa de comandos durante
parada e rearme. Veja os [ensaios do Mestre](mestre.md#limites-e-verificações-em-bancada).
