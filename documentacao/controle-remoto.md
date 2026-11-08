# Preparação de controle remoto do veículo

[Índice](README.md) · [Protocolos](protocolos.md) · [Painéis](satelites.md)

## Escopo atual

O mestre recebe pedidos de marcha, velocidade máxima, aceleração máxima e
**parada remota** enviados pela página Controle da Equipe. Esta revisão apenas
guarda e consulta os pedidos: **não atua sobre motor, freio ou controlador**.
Não há confirmação de imobilização do carro. O pedido de parada não substitui
um dispositivo físico de emergência e a UART/Wi-Fi não garante parada instantânea.

Nenhum comando altera leituras reais, a marcha publicada na telemetria ou o
simulador do veículo. Valores solicitados e valores medidos são conceitos distintos.
Visitantes mantém sua interface pública somente de leitura, sem novos comandos.

## Operação pelo painel

Na página **Controle**, a leitura recente de velocidade, acelerador, freio,
corrente, tensão e potência aparece antes dos comandos. A idade da telemetria
é indicada; campos indisponíveis no modo real não recebem valores simulados.
Os valores solicitados aparecem acima dos formulários. Marcha, limites e
liberação pedem confirmação. **Solicitar parada envia imediatamente, sem
diálogo de confirmação**, acompanhado de aviso discreto. A página Comandos
mantém o histórico e o estado observado. O estado é consultado
automaticamente no ciclo de estados operacionais (aproximadamente 2 s, sujeito
ao tempo das demais consultas UART), além do botão Atualizar estado.

1. Defina os limites desejados de velocidade e aceleração.
2. Libere explicitamente o pedido de parada, caso queira preparar uma marcha.
3. Selecione frente ou ré. Para inverter o sentido, solicite neutro primeiro.
4. Solicitar parada trava o pedido e retorna a marcha solicitada para neutro.
   Liberar a parada mantém neutro; não retoma a marcha anterior.

Sem resposta, os valores anteriores não confirmam o pedido. Dois navegadores
compartilham o mesmo estado no mestre: uma consulta mostra o valor mais recente,
que outro operador pode ter alterado. A revisão identifica alterações, mas não
é um mecanismo de exclusividade ou autorização de operador.

O botão de parada permanece disponível durante outra operação do painel e
envia sua requisição sem esperar a fila de consultas do navegador. Um segundo
toque enquanto o envio está pendente é ignorado. A comunicação e o processamento
no mestre ainda podem atrasar ou falhar; resposta e estado consultado aparecem
separadamente. A interface não apresenta o pedido como frenagem física confirmada.

## Estado, limites e reinício

| Campo | Unidade/faixa de preparação | Inicial |
|---|---|---|
| Marcha solicitada | 0 neutro, 1 frente, 2 ré | Neutro |
| Velocidade máxima solicitada | 0–35 km/h, resolução 0,01 | 0 |
| Aceleração máxima solicitada | 0–5 m/s², resolução 0,001 | 0 |
| Parada solicitada | Travada ou liberada | Travada |
| Atuação habilitada | Sempre falso nesta revisão | Falso |

As faixas são limites do contrato de preparação, não uma certificação dos
limites físicos do carro. Zero significa limite zero, nunca “ilimitado”. Marcha
diferente de neutro exige limites positivos e parada liberada. Zerar qualquer
limite retorna o pedido de marcha a neutro. O estado fica em RAM: reiniciar o
mestre restaura os valores iniciais; não é salvo na NVS nem restaurado do SD.
Não há retomada automática nem alteração de GPIO, PWM, CAN ou saída de freio.

## Contrato HTTP e UART

Todos os POST usam `/api/mestre?consulta=NOME`, com corpo textual contendo
somente um inteiro decimal sem sinal. O servidor HTTP rejeita caracteres extras;
o mestre valida novamente a faixa e as condições de estado.

| Nome HTTP | Ação UART dentro de `COMANDO_EXECUTAR_ACAO_MESTRE` (31) | Campo `valor` |
|---|---|---|
| `definir_marcha` | 17 | 0, 1 ou 2 |
| `velocidade_maxima` | 18 | Centésimos de km/h, 0–3500 |
| `aceleracao_maxima` | 19 | Milésimos de m/s², 0–5000 |
| `parada_remota` | 20 | 1 solicitar; 0 liberar |

Os demais campos da solicitação estruturada devem ser zero. A confirmação
existente de ação significa pedido aceito, não atuação física. Somente a origem
Equipe pode executar essas ações ou consultar o novo estado. O cache de
solicitações existente evita reexecução de retransmissões com o mesmo ID.

`GET /api/mestre?consulta=controle_remoto` usa o comando UART 41, sem carga,
e devolve `estado_controle_remoto_t`, formato 1, com 20 bytes compactados:
quatro campos de 8 bits (formato, atuação, marcha, parada) e quatro campos de
32 bits (velocidade, aceleração, revisão, instante da alteração em ms desde o
boot). A ordem está em `firmware/compartilhado/protocolos/controle_remoto.h`.
Revisão e instante são contadores de 32 bits e podem dar a volta.

O quadro de comandos permanece v1, sem mudar estruturas antigas; a extensão
usa novos IDs e o bit 13 de suporte. Mestres antigos mantêm os controles
desabilitados no painel atualizado. Atualize mestre e Equipe para usar a função;
Visitantes não precisa de atualização por esta funcionalidade.

## Organização e futura integração

`servico_controle_remoto` é o único dono do estado solicitado. Protege atualização
e cópia com seção crítica curta; valida antes de alterar e registra alterações
fora da trava. Não cria tarefa, fila, alocação dinâmica ou dependência de outro
serviço. `servico_comandos` adapta a UART para essa API; o servidor da Equipe
adapta HTTP para UART; a página apresenta o resultado. O contrato compartilhado
contém apenas tipos e constantes, evitando dependências circulares.

A integração física futura deve ser composta pela aplicação, com um serviço de
atuação separado e retorno de estado medido. Antes de habilitá-la será necessário
definir autenticação/autorização, exclusividade e validade temporal dos pedidos,
comportamento na perda de enlace, intertravamentos por velocidade real para
inversão de sentido, limites físicos e confirmação de parada. O AP da Equipe
atualmente não autentica individualmente os operadores. Não basta mudar o campo
`atuacao_habilitada` para tornar este mecanismo adequado a dirigir o veículo.

## Prévia e validação

`python ferramentas/visualizar_painel_equipe.py --rede` oferece os mesmos pedidos
e intertravamentos em memória. Ela permite testar confirmação, limites e mestre
sem resposta; não acessa nenhum ESP. A compilação valida a integração dos
firmwares; comportamento físico não faz parte desta entrega.
