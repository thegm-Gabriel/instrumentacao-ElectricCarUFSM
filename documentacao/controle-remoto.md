# Controle de marcha, acelerador e parada

## Partida, atualização e acionamento

O mestre inicia desarmado. Após pelo menos 5 s de funcionamento, a
inicialização dos serviços e, quando aplicável, a observação OTA, ele arma
automaticamente na marcha indicada pela chave física, limitada ao teto
configurado, e no sentido para frente somente se os sensores entregarem leituras
reais e coerentes, o carro estiver a até 0,5 km/h, houver limites positivos
de velocidade e acelerador, e o pedal permanecer solto por 500 ms. Se a
saída física do DAC for habilitada, o comando de 0 V também precisa ter sido
confirmado no I²C. Enquanto faltar alguma condição, permanece desarmado;
uma falha de controle ou parada acionada pelo operador cancela esse rearme
automático da partida.

Uma instalação OTA de qualquer alvo ativa a parada antes da transferência e
impede o rearme durante ela. Após concluir ou falhar, a parada permanece
ativa até o operador rearmar. A simples busca por atualização não desarma o
carro.

O relé previsto usa o contato **normalmente aberto (NA)**: bobina desligada
significa entrada de habilitação do driver aberta. O código prepara as saídas
de marcha L (modo A), M (nenhum modo), H (modo B), ré e bobina do relé,
desligando o relé antes de mudar os demais sinais e aguardando 100 ms para
religá-lo. Os GPIOs dessas saídas ainda estão indefinidos, e tanto elas quanto
a saída diferente de zero do DAC continuam desativadas por configuração.
Um resistor físico na base do transistor deve garantir bobina desligada
durante reset e antes do firmware iniciar. Sem medir o circuito real, o estado
de software não confirma que o driver foi desabilitado.

Os botões **Salvar velocidade** e **Salvar acelerador** da página Equipe
gravam os limites na NVS do mestre. Eles são restaurados após reinício; o
armamento automático ainda depende das verificações acima.

[Índice](README.md) · [Protocolos](protocolos.md) · [Painéis](satelites.md)

## O que funciona agora

A página **Controle** da Equipe envia ao mestre a marcha máxima (`L`, `M`, `H`),
a permissão de ré, os limites de velocidade e acelerador, a zona morta do pedal
e a parada remota. O mestre lê o pedal Hall no **GPIO3**
pela unidade ADC1, com calibração do ADC; a faixa informada para esse pedal é
**0,5–2,5 V**. A tarefa de controle converte o curso útil em 0–100%, aplica a
zona morta e o limite do acelerador e calcula a referência para o MCP4725.
O DAC é procurado no endereço I²C `0x60` e recebe comando de **0 V** ao
iniciar. A saída diferente de zero permanece **desabilitada por padrão** em
[`veiculo.h`](../firmware/mestre-s3/include/nucleo/configuracao/veiculo.h) até a validação da ligação e do intertravamento físico.
Os GPIOs dos transistores de marcha, reversão e relé continuam indefinidos;
os valores do painel não confirmam a atuação do controlador do veículo.

O botão de parada zera imediatamente a referência de acelerador no estado do
mestre e marca o corte por relé como solicitado. Ainda depende do navegador,
Wi-Fi, UART e
mestre para chegar a ele; **não confirma desaceleração nem substitui a parada
física independente**. A página pública de Visitantes continua somente de
leitura. O simulador de telemetria não recebe esses comandos.

## Operação pelo painel

Na página **Controle**, velocidade, pedais, corrente, tensão e potência recentes
aparecem perto dos comandos. O estado de carga (SoC) aparece quando a simulação
fornece esse valor; ainda não há medição real dele. O resumo de controle mostra
marcha, sentido, limites e o tempo restante da parada. A
resposta da consulta também traz os bits de bloqueio para diagnóstico. A
aceitação de um comando e a consulta posterior são apresentadas separadamente. A página
**Comandos** mantém o histórico de ações; o estado de Controle também é
consultado automaticamente, além do botão **Atualizar estado**.

1. Defina a faixa máxima, um limite positivo de velocidade e um teto do
   acelerador entre 0 e 100%. Zero nos dois limites impede acelerar. A
   aceleração é medida, mas não tem limite configurável neste controle.
2. Aguarde pelo menos 5 s desde a última ativação da parada para usar
   **Rearmar**. O rearme automático pode ocorrer após 30 s, se as condições de
   segurança permitirem. Após OTA, o rearme é manual.
3. Com o pedal em zero e velocidade medida de até 0,5 km/h, escolha uma
   faixa até o limite configurado. `L` pede somente o modo A aterrado; `M` não
   aterra A nem B; `H` pede somente o modo B aterrado. Essas são intenções do
   mestre: os transistores ainda não recebem comando elétrico.
4. Para liberar pedidos de ré, use **Permitir ré** e confirme após 3 s. Na
   mudança de sentido, os intertravamentos exigem pedal em zero e velocidade
   medida de até 0,5 km/h. O sentido é independente de `L/M/H`.
5. A zona morta fica em RAM até novo ajuste ou reinício. O teto percentual é
   salvo na NVS. A tensão mínima padrão é `DAC_TENSAO_MINIMA_SAIDA_MV` em
   `include/nucleo/configuracao/veiculo.h`. Os ajustes de calibração exigem o
   pedal solto. A falta de leitura local recente mantém a referência em zero.

**Parada de emergência** não pede confirmação e é enviada sem aguardar a fila de
consultas do navegador. O mestre zera o acelerador no recebimento do comando,
retira a marcha e o sentido solicitado e marca corte por relé. Após a consulta
confirmar o estado, a página exibe um aviso flutuante próprio para a parada,
sem afirmar que houve atuação física. A parada é
encerrada pelo botão **Rearmar** a partir de 5 s ou pelo rearme automático
após 30 s. A espera de 5 s é aplicada pelo Mestre e recomeça a cada nova
ativação da parada. Uma leitura local do pedal
precisa voltar a zero antes de qualquer autorização futura. O rearme automático
não significa que o
veículo pode voltar a acelerar sozinho.

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
| Reversão autorizada pelo estado | 0 desligada; 1 ré solicitada e sem bloqueio | Desligada |
| Velocidade máxima | 0–35 km/h, resolução 0,01 | 0 |
| Teto do acelerador | 0–100%, resolução 0,1% | 0 |
| Zona morta do pedal | 0–30%, resolução 0,1% | 5% |
| Tensão mínima da saída DAC | 0–2,5 V, resolução 0,001 V | 0 V |
| Pedal lido/acelerador permitido | 0–100%, resolução 0,1% | 0 |
| Parada | Ativa ou encerrada por rearme | Ativa |
| Saída do DAC habilitada | Falsa até validação elétrica | Falsa |

Reduzir a faixa máxima abaixo da selecionada retira a seleção e zera o
acelerador lido e permitido. Alterar o limite de velocidade também zera essa
referência; zero no limite de velocidade ou no teto do acelerador nunca
significa “sem limite”. Quando a velocidade medida alcança o limite, o mestre
zera a referência e exige uma nova leitura do pedal em zero antes de voltar a
permitir aceleração. A queda posterior da velocidade não a restaura. A
aceleração longitudinal continua disponível para telemetria e diagnóstico,
sem ser comparada com um limite ajustável. O valor permitido é o menor entre
pedal lido e teto configurado; sua subida é limitada a 0,5 ponto percentual
por ciclo de 20 ms, com redução imediata.

`reversao_solicitada` registra a escolha frente/ré do operador.
`reversao_acionar_solicitado` só acompanha essa escolha quando o estado está
pronto; é zero durante parada ou qualquer bloqueio. Esse segundo campo também
é apenas um resultado do controle em software, sem comando elétrico no pino.

O campo `estado_operacao` distingue **parado** (parada ativa), **bloqueado**
(intertravamento), **pronto** e **acelerando**. `bloqueios` é uma máscara: seus
bits podem aparecer juntos e ajudam a explicar por que o valor permitido é
zero. A definição numérica fica no cabeçalho compartilhado.

| Bloqueio | Condição |
|---|---|
| Sem faixa | Nenhuma das faixas L/M/H foi escolhida. |
| Sem limites | Velocidade máxima ou teto do acelerador igual a zero. |
| Sem sensores | MPU ou odômetro sem leitura real, recente e coerente. |
| Telemetria simulada | A fonte simulada está ativa. |
| Velocidade | Limite de velocidade alcançado. |
| Pedal indisponível | Amostra local ausente, vencida ou aguardando zero. |
| Falha travada | Sensor falhou enquanto havia pedal ativo. |
| Partida ou OTA | Inicialização incompleta ou instalação OTA em andamento. |

O estado fica em RAM. Reiniciar o mestre restaura parada ativa, faixa nenhuma,
frente, faixa máxima `L`, zona morta de 5%, tensão mínima de
0 V e pedal em zero. Os limites de velocidade e acelerador são restaurados da
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

O botão **Rearmar** envia tecnicamente `parada_remota` com valor `0`.
O Mestre só aceita o rearme manual após pelo menos 5 s desde a última parada,
com o pedal solto, os sensores válidos e o carro quase parado. A marcha vem da chave física e
respeita o limite remoto. Os demais campos da solicitação estruturada devem
ser zero. A confirmação de
ação significa que o mestre aceitou o valor, não que houve atuação física.
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
1 a cada **20 ms**, prioridade **9**, acima das demais tarefas definidas do
projeto. Ao receber o comando de **ativar** a parada, o mestre o processa
diretamente, sem esperar as ações comuns de Wi-Fi, OTA ou Google na fila. Nesse
tratamento já zera a referência de acelerador e retira a faixa; a tarefa
periódica recalcula bloqueios e confirma o estado. O rearme manual segue
o fluxo normal de ações. A tarefa
lê o serviço de aceleração e o odômetro, sem depender do ritmo de atualização
da página ou de operações de SD, Google e OTA. A elevação de prioridade reduz
atrasos causados por essas tarefas, mas a rede e a UART ainda podem retardar a
chegada do comando ao mestre. Não há prazo garantido de ponta a ponta nem corte
físico implementado.

O mestre só permite acelerador diferente de zero com leituras reais recentes e
coerentes: MPU calibrado e sem erro, aceleração renovada há até **300 ms**;
odômetro sem erro, rodas coerentes e velocidade renovada há até **1 s**.
Simulação de telemetria, leitura inválida ou ausente, parada ativa, faixa
ausente, limite zero ou velocidade máxima alcançada mantêm o valor permitido
em zero. A tarefa lê o pedal local normalmente a cada **20 ms**. Se uma leitura
diferente de zero envelhecer por mais de **300 ms**, o mestre
entra em parada; falha dos sensores com pedal ativo também provoca parada.
Ao iniciar, parar, rearmar o controle ou atingir o limite de velocidade, o
serviço exige uma nova leitura de pedal em zero antes de permitir aceleração.
Tensão fora da faixa plausível de **0,3–2,7 V**, erro de ADC ou leitura pausada
pelo terminal também bloqueiam o pedal. Se a falha ocorrer com pedal ativo,
o mestre entra em parada. A zona morta retira o início do curso e reescala o
restante até 100%. Com referência permitida zero, o comando ao DAC é zero;
acima disso, a tensão parte do mínimo configurado e sobe até 5 V segundo o
percentual permitido. A subida é limitada a 25 pontos percentuais por segundo;
a redução é imediata.

O terminal do mestre oferece **Controle do veículo e pedal** no menu 12:
consulta tensão do pedal e estado do DAC e ajusta zona morta, tensão mínima e
acelerador máximo. A página Controle oferece os ajustes de zona morta e teto
do acelerador. A tensão do DAC no painel é a **última referência aceita no I²C**.

`servico_comandos` adapta a UART ao serviço; o servidor da Equipe adapta HTTP
à UART; a página apresenta o resultado. O contrato compartilhado contém apenas
tipos e constantes, evitando dependências circulares. As atualizações e cópias
do estado usam seção crítica curta, sem alocação dinâmica no caminho de
parada. O AP da Equipe aceita ações dos clientes conectados sem identificar um
operador exclusivo; a revisão do estado não substitui autorização e ensaio.

## Integração com o controlador: o que falta

O GPIO3 e o driver do MCP4725 estão implementados. O mestre calcula quando
aterrar A/B, quando cortar pelo relé e qual acelerador permitir, mas a saída
não nula do DAC continua bloqueada; os demais valores não comandam os
componentes. Antes de produzir qualquer sinal para o controlador,
é preciso reconciliar o esquema, a PCB montada e a pinagem do firmware. No
arquivo `hardware/kicad/KiCad_pcb_telemetria/KiCad_pcb_telemetria.kicad_pcb`,
os comandos `DRV_MODE_B_CMD`, `DRV_MODE_A_CMD` e `DRV_ENABLE_CMD` chegam,
respectivamente, aos GPIOs 13, 14 e 21. O firmware atual usa esses mesmos
pinos para LED azul, buzzer e canal B do encoder direito. Essa sobreposição
impede atribuí-los à atuação apenas por alteração de software. Não se deve
reaproveitar silenciosamente esses GPIOs. A revisão de PCB fisicamente
instalada também precisa ser identificada.

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
Não ligue pull-up de 5 V diretamente aos GPIOs do ESP. O GPIO3 é também pino
de configuração de partida do ESP32-S3; confirme que o circuito do pedal não
altera a partida. A entrada ADC nunca deve receber mais que a alimentação
permitida pelo ESP.

O MCP4725 restaura ao ligar o valor de sua EEPROM, inclusive antes da
inicialização do mestre. O firmware **não grava a EEPROM** em operação; ao
iniciar, tenta escrever 0 V no registrador volátil. Isso não substitui um
intertravamento físico que mantenha a tração desabilitada durante boot, perda
de alimentação, travamento do ESP ou falha do I²C. Se uma escrita I²C falhar
enquanto o DAC estiver em valor não nulo, o mestre registra a parada lógica,
mas não consegue comprovar que a saída foi a zero. É necessário validar um
corte físico independente antes de habilitar `HABILITAR_SAIDA_ACELERADOR_DAC`.

Os dois transistores A/B nunca devem conduzir juntos; ao mudar de
faixa, a implementação elétrica precisa retirar o modo anterior e respeitar
o tempo de acomodação medido em bancada antes de aplicar o novo. Falta também
retorno independente do estado do relé e do driver, verificação de limites da
entrada de aceleração, autorização de operador, exclusividade de comando e
teste sob falha de alimentação ou enlace. O campo `atuacao_habilitada` não
pode ser alterado para verdadeiro apenas por configuração; o controle físico
exige hardware corrigido, proteção independente e ensaios com tração isolada.

## Prévia e validação

`python ferramentas/visualizar_painel_equipe.py --rede` apresenta a página
sem acessar os ESPs. Ela permite inspecionar a interface e o fluxo de comandos,
mas não comprova os intertravamentos do mestre. Testes de unidade e compilação
validam somente o contrato de software; nenhum deles substitui ensaio elétrico
e funcional com o controlador isolado da tração.
