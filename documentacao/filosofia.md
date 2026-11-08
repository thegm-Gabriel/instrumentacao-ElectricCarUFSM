# Filosofia do código e fluxos de funcionamento

[Índice da documentação](README.md)

Este guia explica **por que** o software foi dividido dessa forma e o caminho
percorrido pelos dados. Os valores de GPIO, limites e campos binários devem ser
conferidos nos cabeçalhos apontados em [pinagem](pinagem.md) e
[protocolos](protocolos.md); esta página descreve as regras de integração.

## Fronteiras e responsabilidades

| Parte | Conhece | Não deve decidir |
|---|---|---|
| `nucleo` do mestre | Configuração da placa e políticas constantes | Estado de periféricos em execução. |
| `gerenciadores` | ESP-IDF, drivers, filas e acesso físico | Significado de uma leitura do veículo. |
| `dispositivos` | Componente físico, calibração e conversão local | HTTP, Google ou apresentação. |
| `servicos` | Regras da aplicação, estados e recuperação | Detalhes de interface de um cliente web. |
| `main.cpp` / `main.c` | Ordem de partida e composição dos serviços | Lógica duplicada de driver ou protocolo. |
| `firmware/compartilhado` | Formatos UART e serviços comuns aos satélites | Política exclusiva de um painel. |

O mestre é dono da aquisição, do odômetro, dos registros no SD e da decisão de
OTA. Cada satélite é dono do seu ponto de acesso, do servidor HTTP e do último
retrato UART recebido. O satélite da Equipe encaminha comandos ao mestre; o
painel de Visitantes disponibiliza apenas as leituras públicas. Essa separação
permite que um navegador, AP ou satélite falhe sem interromper a aquisição.

As dependências seguem de configuração e contratos para drivers, dispositivos,
serviços e apresentação. Um serviço que precisa de outro recebe uma interface
pública ou um callback composto na inicialização; não se criam dependências
circulares para fazer um botão ou relatório funcionar. O serviço do simulador,
por exemplo, produz o mesmo modelo de telemetria dos sensores reais, sem saber
como a UART ou o navegador funcionam.

## Regras usadas na implementação

1. **Um responsável por recurso.** O serviço de registro é o único escritor dos
   CSVs e logs no SD; o serviço de enlace administra as UARTs do mestre; o
   serviço OTA coordena autorização, download e escolha de partições. Outros
   módulos enviam pedidos ou retratos por filas e interfaces públicas.
2. **Aquisição não espera rede ou disco.** O retrato do veículo é publicado a
   cada 250 ms em operação normal. A cópia histórica é reduzida a 1 Hz e
   enfileirada. HTTPS, FAT, navegadores e apresentação têm seu próprio tempo de
   execução. Durante OTA a taxa UART pode ser reduzida conforme configuração.
3. **Falha é informação, não zero.** O protocolo contém campos que ainda não
   possuem sensor real. A interface usa validade, idade e origem simulada para
   não transformar ausência em uma medição aparente. O painel distingue comando
   aceito, execução em andamento e resultado observado depois.
4. **Estados explícitos e erros preservados.** Gerenciadores retornam o
   `esp_err_t` original. Serviços classificam recuperação ou degradação;
   apresentação lê o diagnóstico já agregado. Inicializações com várias etapas
   desfazem recursos criados se uma etapa falhar. Uma chamada concluída pode ser
   repetida sem criar tarefas ou drivers duplicados.
5. **Repetição segura.** Quadros UART têm versão, tamanho e integridade. Um
   comando com o mesmo identificador recebe a resposta memorizada quando
   reaparece, evitando executar duas vezes uma ação com efeito colateral. Blocos
   OTA usam sessão, sequência e deslocamento para rejeitar repetição indevida.
6. **Memória conforme o uso.** Buffers DMA e quadros UART permanecem em memória
   compatível com os drivers. A PSRAM pode guardar histórico de diagnóstico e
   buffers reutilizados de download OTA; há caminho reduzido em RAM interna
   quando ela não está disponível. Não se aloca um novo bloco a cada amostra.
7. **Configuração e segredo têm destinos distintos.** Limites físicos ficam em
   cabeçalhos `configuracao_*.h`; preferências persistentes ficam na NVS.
   `segredos_ota.h` é local e ignorado pelo Git. O retrato de configuração no SD
   exclui senhas. Na partida, ele recupera o odômetro, a política Wi-Fi, a
   preferência de conexão automática e o intervalo OTA se os respectivos dados
   NVS estiverem ausentes. Também repõe os intervalos e a pausa dos relatórios
   antes de iniciar esse serviço. Nomes de redes são informativos e não recriam
   perfis sem credenciais.

## Sequência: leitura até as duas páginas

1. Encoders, ADC e MPU6050 produzem leituras no mestre. O odômetro calcula
   velocidade e percurso; a origem simulada, quando ativada, preenche o mesmo
   modelo em unidades de engenharia.
2. `servico_telemetria` compacta o modelo no quadro v4, identifica sequência e
   instante do mestre, aplica CRC-16 e entrega cópias às duas filas UART. O
   produtor não espera o TX físico ou a página web.
3. Cada receptor separa telemetria, comandos e OTA pelo enquadramento. Só publica
   uma amostra nova após validar cabeçalho, versão, tamanho, CRC, plausibilidade
   e sequência. Uma retransmissão duplicada recebe ACK, mas não renova a idade
   da amostra nem aumenta o contador de amostras válidas. O mestre mede RTT,
   perdas e idade da resposta por satélite.
4. O servidor de cada AP lê o retrato publicado. O navegador da Equipe consulta
   `/api/telemetria` a 1, 2, 3 ou 4 Hz, escolhido na página; Visitantes consulta
   a 2 Hz. Essa é a frequência de consulta, que pode cair se a rede ou o
   navegador atrasar. Amostras antigas permanecem identificadas pela idade.
5. O registro histórico recebe a cópia a 1 Hz. O CSV ativo é sincronizado a
   cada 10 s e fechado por duração ou tamanho. Os gráficos e o CSV exportado
   pelo navegador pertencem apenas à sessão da página, não são o arquivo do SD.

## Sequência: comando da Equipe

1. O operador envia uma ação na página **Comandos**. O JavaScript faz a
   confirmação visual quando necessário e chama `/api/mestre?consulta=...`.
   O servidor valida método, consulta e corpo antes de encaminhar pela UART.
2. A solicitação usa um identificador. O mestre valida o quadro CRC-32 e sua
   carga, executa a operação permitida ou devolve um resultado explícito.
3. A resposta confirma a aceitação ou recusa da solicitação. Para OTA,
   sincronização Google e outras operações demoradas, isso **não** significa
   conclusão: o painel consulta o estado posterior e mostra etapas, progresso
   e último resultado. Sem resposta, a interface indica o prazo esgotado.

O AP da Equipe mantém a senha Wi-Fi compartilhada com o AP dos visitantes, mas
as ações HTTP exigem uma chave administrativa própria, gerada na primeira
partida e guardada na NVS. A chave aparece no terminal serial do ESP da Equipe;
o navegador a guarda apenas durante a sessão. Consultas e telemetria continuam
legíveis sem essa chave. As validações do mestre seguem aplicáveis. Consulte a
tabela de rotas e corpos em [satélites](satelites.md).

## Sequência: falha e observabilidade

O diagnóstico reúne memória livre e maior bloco, CPU, margens de pilha,
contadores de sensores e UART, estados dos satélites e fases de tarefas. A
supervisão detecta prazos excedidos; outra tarefa executa recuperação que possa
bloquear. Problemas opcionais, como OLED ou SD ausente, são reportados e não
devem parar a UART de telemetria.

Em funcionamento normal, o SD recebe um resumo de diagnóstico a cada 30 s.
Uma tarefa guarda as amostras recentes em PSRAM (ou um histórico menor em RAM
interna). Ao detectar uma falha, o serviço grava o contexto e o histórico
anterior, depois aumenta temporariamente a cadência para observar instabilidade
posterior. Falhas graves usam janela mais longa. O log registra tempo desde o
boot e UTC apenas depois de obter um cabeçalho HTTPS `Date` válido. Consulte
[mestre](mestre.md#armazenamento-e-envio-ao-google) para arquivos, limites e
comportamento quando o SD falha.

## Ao alterar o código

| Mudança | Verificações necessárias |
|---|---|
| Campo ou enquadramento UART | Atualizar o contrato em `firmware/compartilhado/protocolos`, os três consumidores, [protocolos](protocolos.md) e testes de parser/CRC. |
| GPIO, escala física ou montagem | Atualizar `configuracao_placa.h`, [pinagem](pinagem.md) e validar em bancada. |
| Comando ou resposta | Preservar identificador, resultado e tamanho; revisar servidor HTTP, estado assíncrono e [satélites](satelites.md). |
| Tela web | Editar HTML/CSS/JS em `web/`, executar o respectivo `gerar_painel.py` e revisar celular e computador. O `.c` gerado acompanha a fonte no Git. |
| Tarefa ou buffer | Medir pilha, maior bloco interno, PSRAM e tempo sob Wi-Fi/OTA antes de mudar prioridades ou afinidade. |
| Versão ou publicação | Seguir [versionamento](versionamento.md) e [OTA](ota.md); não alterar somente o número exibido na interface. |

Compilar valida integração e tamanho da imagem. Os testes automatizados cobrem
contratos específicos; comportamento elétrico, cobertura Wi-Fi, queda de energia
e temporização no veículo ainda exigem ensaio de bancada.
