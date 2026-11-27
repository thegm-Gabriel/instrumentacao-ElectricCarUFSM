# Guia técnico dos satélites e das APIs locais

[Índice da documentação](README.md)

Este guia descreve os dois satélites do NEXUS, em `firmware/equipe-s3` e
`firmware/visitantes-s3`. Ambos usam ESP32-C3 SuperMini Plus com 4 MiB de
flash, um núcleo RISC-V e dois slots OTA. Os projetos PlatformIO/ESP-IDF usam
o ambiente `esp32-c3-supermini-plus`. Consulte o [guia de primeira gravação](ota.md#primeira-gravação-por-cabo). Os contratos de telemetria, comandos e OTA
ficam em `firmware/compartilhado/protocolos`. O logotipo comum fica em
`firmware/compartilhado/web`.

Cada satélite inicializa os próprios módulos em `src/main.c`. O gerenciador
Wi-Fi cuida do AP e das sessões dos clientes. O gerenciador UART instala e
opera o driver com os buffers e prazos do projeto. O receptor separa os
quadros e os encaminha aos serviços locais de telemetria, comandos e OTA.
A Equipe serializa pedidos da página web ao Mestre e registra métricas do
driver UART. Visitantes responde aos pedidos recebidos pelo enlace e publica
a telemetria na sua página. Os arquivos `src/CMakeLists.txt` listam as fontes
de cada projeto.

## Fluxo comum

1. O mestre publica um quadro binário de telemetria a cada 250 ms em operação
   normal. O enlace UART é 3,3 V, 8N1, 1.000.000 baud, com TX, RX e GND ligados
   nos dois sentidos. A [pinagem](pinagem.md) traz os GPIOs.
2. `servicos/receptor_uart.c` recebe o fluxo e separa telemetria, comandos e OTA.
   Só publica um retrato novo depois de conferir início, versão, tipo, tamanho,
   CRC-16, plausibilidade e sequência. Uma retransmissão da mesma sequência
   recebe ACK, mas não substitui a amostra, renova sua idade ou aumenta o
   contador de amostras válidas. Registra CRC, cabeçalho, conteúdo, duplicatas,
   perdas e reinícios do mestre. Um contador de tempo do mestre retrocedendo
   provoca resincronização da sequência.
3. Cada pacote aceito produz um ACK de 20 bytes com sequência, tempo do mestre,
   identidade e versão do satélite. O mestre usa esse retorno para
   acompanhar a saúde dos enlaces. Quadros de comandos e OTA também têm respostas
   pelo mesmo TX do satélite.
4. O retrato da API permanece disponível depois de uma interrupção do enlace.
   Consulte `valido` e `idade_ms` para distinguir ausência inicial de dados e
   uma amostra antiga; a interface sinaliza desatualização.

Ao iniciar, ambos deixam uma imagem OTA nova em observação por 2 min após UART,
Wi-Fi e HTTP funcionarem. Se reiniciar antes da confirmação, o bootloader
retorna à imagem anterior. Três reinícios por panic ou watchdog na janela de
5 min após a partida também solicitam rollback. A contagem usa memória RTC e
é perdida após corte total de energia. A equipe inicializa diagnóstico local
como recurso complementar.

## ESP32-C3 da Equipe

`include/nucleo/configuracao.h` define o AP `NEXUS-Equipe`, canal 11,
até cinco clientes e a UART1 em GPIO 5 (RX) e 6 (TX). A rede padrão do ESP-IDF
usa `192.168.4.1` e DHCP. O firmware anuncia o portal HTTP pela opção DHCP 114,
quando suportada. Ele não oferece servidor DNS local; acesso confiável:
`http://192.168.4.1/`.

O ponto de acesso usa a mesma senha Wi-Fi configurada no firmware de Visitantes.
Clientes conectados ao AP da Equipe podem enviar comandos HTTP; não há chave
administrativa adicional. A permissão validada no mestre continua necessária
para alterar a rede principal e a política Wi-Fi.

`web/index.html`, `web/style.css`, `web/layout.css` e `web/app.js` são as fontes
do painel NEXUS. `web/gerar_painel.py` gera `src/servicos/painel_web.c`,
incorporado ao binário. As rotas `/`, `/painel`, `/diagnostico` e `/mestre`
entregam o mesmo painel com sete abas.

| Página da Equipe | O que apresenta ou permite fazer |
|---|---|
| Telemetria | Velocidade, pedais, energia disponível, leituras do MPU6050/INA228, histórico e exportação da sessão. |
| Controle | Marcha máxima, permissão de ré, limites de velocidade e de pedal, zona morta, parada e rearme. Mostra o estado lógico informado pelo Mestre. |
| Comandos | Diagnóstico de sensores, simulação, Wi-Fi do mestre, clientes dos APs, Google, OTA, logs e histórico das ações. |
| Mestre | Saúde, causas de degradação, memória, CPU, tarefas e estado obtido por consultas UART. |
| Equipe | Diagnóstico local, recepção UART e estado do ponto de acesso técnico. |
| Visitantes | Diagnóstico remoto, disponibilidade, memória, CPU e clientes quando o satélite responde. |
| Sobre | Intenção do projeto, símbolo, orientação e equipe discente. |

As páginas dos ESPs mostram o estado do enlace, CPU, memória interna e
temperatura. Os detalhes técnicos e o histórico podem ser expandidos. A aba
Mestre também mostra as causas de degradação e a idade do diagnóstico.
No celular, **Dispositivos** reúne Mestre, Equipe, Visitantes e Sobre.
Os contadores das duas redes ficam em **Comunicação e sessão**, na telemetria.
Em **Comandos**, os atalhos levam aos grupos **Diagnóstico**, **Rede**,
**Operação** e **Atividade**. Em **Sensores**, é possível consultar leituras e
pausar ou retomar a aquisição, por sensor ou em conjunto. Com a atuação física
habilitada, essa seção fica disponível para consulta. Os ajustes de Wi-Fi e do
terminal protegidos exigem a senha de permissão. Os controles novos dependem de
firmwares compatíveis no mestre e na Equipe.
Cada sensor mostra se está lendo, pausado ou sem leitura válida. Os detalhes
reúnem os valores por grandeza e os contadores do barramento I²C. A recuperação
do MPU6050 e do INA228 ocorre no serviço de aquisição, em condições específicas
de falha. A supervisão trata a recuperação de tarefas por módulo.

Em **Sensores**, o cartão **Acelerador** mostra a tensão do pedal no GPIO5,
a posição lida, o acelerador permitido e a última referência aceita pelo
MCP4725. A confirmação de zero é a resposta do DAC à escrita por I²C.
O quadro de seletores mostra a marcha física, o sentido selecionado e se o
Mestre ativou o comando de ré.
Enquanto **Sensores** estiver aberto, essas leituras acompanham a taxa de
1, 2, 3 ou 4 Hz selecionada em **Telemetria**.

**Tarefas do mestre** mostra CPU, pilha, prioridade e estado. Cada página reúne
até cinco tarefas da mesma coleta. **Logs do terminal** permite ajustar o nível
global ou por fonte, o perfil NEXUS e as cores do serial.
Em **Wi-Fi do mestre**, a rede principal pode ser definida a partir de SSID e
senha novos ou de um perfil já salvo. O mestre testa a conexão antes de gravar;
uma rede promovida sai da lista de secundárias. O painel consulta novamente a
configuração e mostra o SSID principal confirmado. A seção também permite
testar e salvar redes abertas ou protegidas, conectar ou excluir perfis,
restaurar a principal do firmware, configurar conexão automática e ajustar
tentativas e tempos ou restaurar a política padrão. A consulta de estado e a lista de redes podem ser
atualizadas pelo botão do próprio cartão.
**Consultas** mostra os campos principais da resposta com rótulos e unidades.
O detalhe **Resposta completa (JSON)** reúne todos os campos recebidos.
O indicador de saúde no cabeçalho mostra a causa ativa e abre o diagnóstico
do Mestre. As causas conhecidas têm atalhos para o detalhe correspondente.
Uma falha ativa também gera um aviso flutuante com o mesmo atalho.
Em **Controle**, a mensagem distingue consulta em andamento, falta de
resposta do Mestre e firmware que informa que o recurso é indisponível.
A telemetria pode ser consultada a 1, 2, 3 ou 4 Hz
(padrão de 4 Hz, compatível com os 250 ms da publicação do mestre). As consultas
de estado OTA, Google e simulador continuam automáticas a cada 2 s,
independentemente dessa escolha. O cartão Google também permite iniciar a
sincronização dos arquivos pendentes e acompanhar o progresso informado pelo
mestre. Uma requisição lenta ou falha pode reduzir a taxa efetiva no navegador.
Gráficos e CSV do navegador acumulam dados apenas enquanto a página está aberta.
O CSV usa o horário local do navegador com o deslocamento de fuso em cada amostra.
Os gráficos combinam séries no mesmo intervalo de tempo, com duas escalas e
unidades nas laterais. Movimento mostra velocidade (escala inicial de 0 a 30 km/h,
expandida quando uma amostra excede esse valor e restaurada depois)
e aceleração; o gráfico elétrico permite escolher duas grandezas com eixos
laterais e uma terceira curva opcional, tracejada, com escala própria indicada
no gráfico. As curvas mantêm cores distintas por eixo. A velocidade e a
grandeza à esquerda no gráfico elétrico têm área preenchida entre leituras
válidas. Passar o mouse ou tocar no gráfico mostra os valores da amostra. Os
diagnósticos do mestre e da equipe começam com CPU e memória livre, enquanto
Visitantes mostra latência e disponibilidade das consultas; seu diagnóstico
também oferece CPU, temperatura e memória quando a consulta ao satélite responde.
Ausência de leitura é exibida como lacuna, sem inventar pontos. O instrumento
da telemetria destaca a velocidade em um arco e apresenta acelerador e freio
em dois medidores separados quando esses campos estão válidos. Um pequeno
segmento curvo coral é desenhado sobre a faixa do arco e desaparece após cerca de
1,4 s quando uma amostra nova mostra queda de pelo menos 1,5 km/h e taxa de
redução de pelo menos 3,5 km/h por segundo, partindo de uma velocidade recente
de 5 km/h ou mais. Repetições da mesma amostra não contam; após o evento há 4 s
de intervalo e é preciso voltar a acelerar pelo menos 2 km/h antes de outro
marcador. A escala do arco mantém a posição do pico até o segmento desaparecer.
É uma indicação visual de desaceleração percebida no navegador, não o limite de
velocidade configurado nem um detector de frenagem.
O arco usa um gradiente espacial de verde para âmbar e coral: cada posição tem
uma cor fixa, de modo que o trecho já preenchido não muda de cor quando a
velocidade sobe. Marcas menores indicam passos de 5 km/h e marcas maiores,
10 km/h. O pico também aparece brevemente no canto superior do velocímetro.
O número ganha tom âmbar durante uma queda perceptível de velocidade; isso representa apenas
a tendência entre leituras. O segmento de pico fica estático quando o navegador
pede movimento reduzido.

O arco começa em 0–30 km/h. A escala só sobe em passos de 5 km/h quando a
velocidade ultrapassa o máximo atual em mais de 0,15 km/h; ela não aumenta
antecipadamente ao se aproximar de 30. Para voltar, a leitura (e eventual pico
visível) deve ficar pelo menos 7 km/h abaixo do máximo por 4 s contínuos.
Então a escala escolhe de uma vez o menor passo de 5 km/h com 1,5 km/h de
folga, nunca menos de 30. Isso evita alternância perto do limite e preserva a
posição do marcador enquanto aparece.

Os indicadores principais suavizam a passagem entre amostras novas, inclusive
velocidade, aceleração, carga, potência, acelerador e freio. O navegador estima
o intervalo real de chegada das sequências (inicialmente 250 ms) e usa uma
transição curta com desaceleração no fim, limitada a 230–730 ms. Respostas
repetidas não reiniciam a animação; numa aba oculta ou com preferência por
movimento reduzido, o valor é aplicado diretamente. A escala do arco e a
detecção de pico usam a leitura recebida, enquanto desenho e número acompanham
o valor interpolado. Isso evita que a animação invente eventos de pico.

A barra de potência usa uma escala visual de 0 a 1000 W e preenche da
esquerda para a direita conforme a magnitude. O sinal e a cor distinguem consumo
de regeneração. Um traço indica que a leitura está indisponível.

A página Sobre explica o projeto Carro Elétrico UFSM-CS, identifica orientação e equipe discente
e apresenta o símbolo do carro. O painel público de Visitantes também traz essa
identidade e os créditos. A imagem fonte fica em
`firmware/compartilhado/web/logo_carro_eletrico.png` e é incorporada ao HTML
pelos dois geradores, sem requisição adicional no navegador.

Os scripts `visualizar_painel_equipe.py` e `visualizar_painel_visitantes.py`
oferecem um menu com acesso só no computador ou também em dispositivos na
mesma rede. Pela linha de comando, `--local` restringe o acesso ao PC e
`--rede` mostra no terminal os endereços para celular e outros computadores.
A prévia da Equipe usa telemetria, comandos e cenários de saúde simulados.
Veja [Prévias da Equipe e de Visitantes](ferramentas.md#prévias-da-equipe-e-de-visitantes).

O painel da Equipe exibe a quantidade de dispositivos conectados aos pontos de
acesso Equipe e Visitantes. As listas de endereço MAC, tempo conectado e ações de
desconexão ficam na página Comandos, separadas da rede externa do mestre usada
para OTA e Google. O limite de sessão Wi-Fi é 15 minutos por padrão, pode ser alterado
ou desativado com 0, e desconecta a estação ao expirar. Ele não altera o prazo
do DHCP nem impede que um dispositivo com a senha salva se reconecte.

| Método e rota | Função |
|---|---|
| `GET /api/telemetria` | Última amostra do veículo, idade, sequência e métricas de recepção UART. |
| `GET /api/diagnostico` | Versão e estado da equipe, Wi-Fi, heap/PSRAM, CPU, tarefas, temperatura, contadores UART, comandos e OTA. |
| `GET /api/wifi/clientes` | Clientes conectados ao AP Equipe, tempo de sessão e limite configurado. |
| `POST /api/wifi/limite` | Define o limite de sessão do AP Equipe em minutos. |
| `POST /api/wifi/desconectar` | Desconecta um cliente do AP Equipe pelo endereço MAC. |
| `GET /api/mestre?consulta=...` | Consulta o mestre pela UART e transforma a resposta em JSON. |
| `POST /api/mestre?consulta=...` | Solicita uma ação ao mestre; a resposta confirma o recebimento/aceitação, e ações assíncronas exigem consulta posterior do estado. |

As rotas `POST` podem ser usadas por clientes conectados ao AP da Equipe. Até
duas consultas simultâneas ao mestre e uma ação
simultânea podem ocupar tarefas de resposta; uma nova chamada recebe HTTP 503
quando a respectiva capacidade está ocupada.

O painel usa HTTP local, sem TLS. Proteja o acesso ao AP da Equipe, pois a
senha da rede é o controle de acesso aos comandos. Alterações protegidas de
Wi-Fi e ajustes de logs também usam a senha de permissão do mestre.

Consultas GET aceitas: `ping`, `versao`, `saude`, `telemetria`,
`uart`, `ota`, `sincronizacao`, `wifi`, `diagnostico_mestre`, `configuracao`,
`simulador`, `sensores`, `logs_terminal`, `tarefas_mestre` (com `inicio`
opcional em passos de 3, de 0 a 45),
`rede_wifi_salva` (com `indice` de 1 a 5), `satelite_equipe_ping`,
`satelite_equipe_versao`, `satelite_visitantes_ping` e
`satelite_visitantes_versao`.

Ações POST aceitas: `verificar_ota`, `autorizar_mestre`, `autorizar_equipe`,
`autorizar_visitantes`, `autorizar_todos`, `cancelar_ota`, `resumo_terminal`,
`alternar_relatorios`, `zerar_percurso`, `sincronizar_google`,
`intervalo_relatorio`, `intervalo_ota`, `intervalo_telemetria_ota`,
`simulador_telemetria`,
`wifi_preparar`, `wifi_conectar_preparada`, `wifi_salvar_preparada`,
`wifi_conectar_salva`, `wifi_excluir_salva`, `wifi_automatico`,
`wifi_alterar_principal`, `wifi_principal_salva`,
`wifi_restaurar_principal`, `wifi_politica`, `wifi_restaurar_politica`,
`sensores` e `logs_terminal`.
Algumas ações recebem campos de texto separados por quebra de linha no corpo
HTTP; a ordem e as validações estão em `preparar_acao_mestre` no
`src/servicos/servidor_web.c`. O painel já prepara esses campos e pede confirmação
antes do envio. Respostas retornam `ok`, código, identificador de solicitação,
tentativas e duração, ou um erro. Requisições com método ou consulta incompatível
são rejeitadas. A fila de comandos evita disputas com a recepção UART.

| Ação POST (`consulta`) | Corpo `text/plain` |
|---|---|
| Verificação/autorizações/cancelamento OTA, `resumo_terminal`, `alternar_relatorios`, `zerar_percurso`, `sincronizar_google` | Vazio. |
| `intervalo_relatorio` | Grupo de 0 a 3, quebra de linha e intervalo numérico. |
| `intervalo_ota`, `intervalo_telemetria_ota` | Um intervalo numérico; o mestre valida a faixa. |
| `simulador_telemetria` | `1` ativa ou `0` desativa até o próximo reinício. |
| `wifi_preparar` | SSID, quebra de linha e senha da rede. Senha vazia representa rede aberta; outras precisam de 8 a 63 caracteres. Prepara a próxima operação. |
| `wifi_conectar_preparada`, `wifi_salvar_preparada` | Vazio; exigem uma rede preparada. |
| `wifi_conectar_salva`, `wifi_excluir_salva` | Índice de 1 a 5, conforme a lista consultada. |
| `wifi_automatico` | `0` ou `1`, quebra de linha e senha de permissão. |
| `wifi_alterar_principal`, `wifi_restaurar_principal` | Senha de permissão; a alteração exige rede previamente preparada. |
| `wifi_principal_salva` | Índice da rede salva (1 a 5), quebra de linha e senha de permissão. O mestre testa a conexão, torna a rede principal e a remove da lista de secundárias. |
| `wifi_politica` | Cinco linhas: tentativas por rede, tempo por tentativa em ms, intervalo entre tentativas em ms, limite da sessão em ms e senha de permissão. |
| `wifi_restaurar_politica` | Senha de permissão. Restaura os quatro parâmetros de tentativas e tempos. |
| `sensores` | ID (0 ADC, 1 MPU6050, 2 encoders, 3 INA228, 4 temperatura do ESP, 255 todos), quebra de linha e `1` para retomar ou `0` para pausar. |
| `logs_terminal` | Quatro linhas: tipo, valor, alvo e senha de permissão. O tipo é `0` nível global, `1` perfil, `2` cores ou `3` nível por fonte. O alvo é o ID da fonte no tipo `3` e `0` nos outros. Valores de nível vão de `0` (silenciado) a `3` (informações). Perfil e cores usam `0` ou `1`. |

O HTTP rejeita consulta ausente ou desconhecida e método incompatível com
`400` ou `405`. Consultas e ações aceitas são
encaminhadas pela UART; conexão a uma rede Wi-Fi pode aguardar até cerca de
185 s, enquanto consultas de satélite usam prazo menor. A confirmação visual
antes do envio pertence ao JavaScript do painel. `ok:true` indica que a
resposta de comando foi aceita;
para OTA, upload e outras ações assíncronas, consulte o estado depois.

O botão de sol ou lua no cabeçalho alterna entre os temas claro e escuro.
O painel abre no tema escuro por padrão e guarda a escolha no navegador.

O painel não recupera sessões passadas do SD nem confirma sozinho o efeito final
de uma operação assíncrona. Tensão do barramento, corrente e potência usam
leituras do INA228 quando o mestre marca a medição elétrica como válida. Nos
indicadores principais, a Equipe mostra estado de carga, potência e corrente
nessa ordem; a aceleração fica com as leituras do MPU6050 em **Operação e
sensores**. O cartão de carga mostra **Atenção** abaixo de 50%,
**Bateria baixa** abaixo de 30% e **Crítico** abaixo de 10%. O cartão de
corrente usa uma escala visual até 35 A e começa a ganhar
tons vermelhos acima de 7,5 A; a cor é apenas um destaque do painel, não um
limite de proteção. A barra de potência cresce da esquerda para a direita em
uma escala visual de até 1000 W. Seu destaque aumenta com a intensidade do
valor e distingue consumo de regeneração. As barras de corrente e potência
mostram por cerca de um segundo uma marca discreta quando a leitura cai após
um pico, com o valor no canto do cartão; ambos desaparecem gradualmente e não
representam um limite de segurança.
O grupo INA228 em **Operação e sensores** mostra **Tensão no shunt**,
potência instantânea, temperatura do chip, energia e carga acumuladas desde o
reset. O diagnóstico do componente fica nas consultas de sensores. A potência
conserva o sinal da corrente. Carga percentual, autonomia e pedais da
telemetria geral são simulados na versão atual. O estado do controle do Mestre
informa separadamente a leitura física local do pedal. O navegador integra
a energia para calcular o consumo médio.
`tensao_adc_bruta` é apenas a contagem não calibrada do ADC.

## ESP32-C3 de Visitantes

`include/nucleo/configuracao.h` define o AP `NEXUS-Visitantes`, canal 1,
até dez clientes, largura solicitada de 20 MHz e potência máxima solicitada de
16 dBm, além da UART1 em GPIO 5 (RX) e 6 (TX). Canal, largura e potência podem
ser alterados por flags de compilação dentro dos limites do cabeçalho. O driver
registra os valores efetivamente aplicados. O AP usa `192.168.4.1`, DHCP e DNS
cativo local opcional: quando ativo, responde consultas IPv4 com o IP do painel.
O acesso por IP continua funcionando se a tarefa DNS não iniciar.
O servidor redireciona caminhos HTTP desconhecidos para `http://192.168.4.1/`,
inclusive os usados por alguns sistemas para detectar um portal cativo; a
abertura automática ainda depende do comportamento do celular. Um pedido a
uma rota inexistente da API também recebe esse redirecionamento.

### QR codes de acesso

Os QR codes em [`artefatos/qr_visitantes`](../artefatos/qr_visitantes) facilitam o acesso pelo celular:

- [`wifi_visitantes.png`](../artefatos/qr_visitantes/wifi_visitantes.png) conecta à rede do ESP de visitantes;
- [`painel_visitantes.png`](../artefatos/qr_visitantes/painel_visitantes.png) abre `http://192.168.4.1/` depois da conexão.

Há versões SVG para impressão. Se o SSID, a senha ou o IP mudarem, execute
`python ferramentas/gerar_qr_visitantes.py` a partir da raiz do repositório
para recriar os quatro arquivos. O gerador requer `qrcode[pil]`
(`python -m pip install "qrcode[pil]"`) e lê os valores diretamente de
`firmware/visitantes-s3/include/nucleo/configuracao.h`. O QR da rede contém
a senha Wi-Fi; distribua-o somente ao público autorizado a conectar-se.

`web/index.html`, `web/style.css`, `web/app.js`, `web/logo_carro_eletrico.webp` e `web/carro_eletrico.webp` são incorporados em
`src/servicos/painel_web.c` por `web/gerar_painel.py`. Há duas rotas:
`GET /` para a página pública e `GET /api/telemetria` para o JSON. Não há API
de diagnósticos ou de comandos nessa rede. O endpoint publica validade, idade,
sequência, origem simulada, velocidade, aceleração, percentuais de acelerador e
freio, potência e percurso, com
unidades codificadas nos nomes dos campos. Aceleração indisponível ou grandezas
elétricas não adquiridas são `null`, não zero. O histórico dos gráficos e
o cálculo do consumo médio começam quando a página é aberta.

### Leitura e apresentação pública

O cabeçalho usa o símbolo do NEXUS da Equipe e mostra o estado das leituras.
O botão de tema alterna entre claro e escuro, inclusive nos gráficos. O tema
inicial é escuro e a escolha fica salva no navegador.
O painel público destaca velocidade, carga da bateria e odômetro. A
aceleração permanece no gráfico de movimento. Campos com disponibilidade
explícita aparecem como **Sem leitura** quando a medição está ausente. Uma
velocidade inválida ainda pode chegar como zero no protocolo atual. Confira
a idade da amostra e o odômetro ao investigar. Os botões **i** dos cartões
abrem descrições por toque, clique ou teclado.

O velocímetro destaca o valor numérico e acompanha a leitura em uma barra
com **escala visual de 0 a 35 km/h**. Acima dessa faixa, a barra permanece
cheia e o número continua mostrando a leitura. Uma queda de pelo menos
0,18 km/h muda temporariamente o tom do número.

Após um pico recente de pelo menos 5 km/h, uma queda de 1,4 km/h ou mais a
uma taxa de pelo menos 2,3 km/h por segundo marca a posição do pico na barra.
A marca aparece brevemente e desaparece em 1,6 s. O velocímetro continua
mostrando a velocidade atual.
Há intervalo de 3,8 s entre eventos, e o marcador só é rearmado depois de
nova subida de pelo menos 2 km/h acima do vale. Amostras antigas ou inválidas
reiniciam o rastreamento. O marcador representa uma redução observada no
navegador; não informa a velocidade máxima configurada para o veículo.

As barras de acelerador e freio crescem da extremidade externa para o centro,
respectivamente da esquerda para a direita e da direita para a esquerda.
`acelerador_decimos_percentual` e `freio_decimos_percentual` usam décimos de
por cento. Como o firmware atual só obtém esses valores na simulação, no modo
real são `null`. O cartão de carga usa uma barra em degradê verde e mostra
a estimativa simulada, sem avisos por faixa de carga.

Quando há potência, a barra usa escala de 0 a 1000 W e cresce com a magnitude.
O valor e o rótulo distinguem consumo de regeneração, que tem cor própria.
Um risco marca por 1 s um pico de pelo menos 120 W seguido de queda de pelo
menos 55 W ou 25%, o maior dos dois, com intervalo de 2,5 s entre marcas.
O cartão aparece quando há leitura elétrica.

O mestre publica telemetria aos satélites nominalmente a cada 250 ms (4 Hz);
o navegador de Visitantes consulta `/api/telemetria` a cada 500 ms (2 Hz), sem
ajuste de taxa. Ele aceita sequências novas ou mudança da origem dos dados e
estima o período de chegada das leituras. Números e barras se aproximam do alvo
com desaceleração no fim:
a transição usa cerca de 1,15 vez esse período, limitada a 450–850 ms. Uma
leitura nova interrompe a transição a partir do valor exibido. Movimento
reduzido solicitado pelo navegador ou aba oculta aplica o valor imediatamente.
Mais de 2,5 s sem leitura nova abre uma lacuna nos gráficos.

O gráfico **Movimento** une velocidade e aceleração na mesma linha de tempo,
com velocidade em azul e aceleração em coral. Cada grandeza tem escala e
unidade próprias nas bordas esquerda e direita. A velocidade
começa em 0–30 km/h e expande o eixo quando as leituras visíveis exigem; isso
não altera a escala fixa da barra do velocímetro. No gráfico **Sistema elétrico**,
cada eixo permite escolher tensão, corrente, potência, carga da bateria,
autonomia restante ou consumo médio. O par inicial é potência e corrente.
Selecionar a grandeza que já está no outro eixo troca as duas de posição.
O eixo esquerdo usa azul-ciano e área preenchida. O direito usa âmbar.
O gráfico indica as séries sem leitura. O toque ou ponteiro mostra o valor da amostra.
Os dois gráficos preservam lacunas quando faltam leituras e usam janelas de
**1, 5 ou 10 minutos**. O eixo horizontal indica o tempo em relação ao momento
atual. O histórico existe enquanto a página está aberta.

O Sobre apresenta o projeto, a orientação e a equipe discente.
A tipografia dos cartões largos cresce no computador sem ampliar os
cartões estreitos do celular; não há fontes ou bibliotecas carregadas da rede.

A seção **Energia** reúne tensão da bateria e corrente elétrica, exibidas com
duas casas decimais, **autonomia restante** em km e **consumo médio** em Wh/km.
A abertura apresenta o nome e uma arte do protótipo do Carro Elétrico UFSM-CS.
A arte recortada e o emblema em **Sobre** são incorporados em WebP.
A faixa de pista se move em proporção à velocidade recebida. Ela pausa
com o carro parado, quando os dados ficam desatualizados, com a aba oculta
ou quando o navegador solicita movimento reduzido.
O indicador no cabeçalho, ao lado do NEXUS, distingue
leitura atual, dados simulados, dados desatualizados e reconexão.
O velocímetro tem destaque. Potência instantânea, carga e odômetro aparecem
em uma faixa no computador e em linhas no celular. A autonomia, recebida no campo `autonomia_decimos_km`,
é uma estimativa do mestre. Atualmente só existe no modo simulado e é `null`
no modo real. A prévia local fornece esse campo para experimentar a interface.
O consumo médio é calculado pelo navegador, dividindo a energia positiva
integrada pela distância dos mesmos intervalos válidos. Inclui energia gasta
parado e não desconta regeneração. Só aparece após 20 m de distância acumulada
com leituras de potência e percurso. Lacunas de comunicação, falta de potência
ou de percurso não acrescentam distância ao denominador; intervalos acima de
3 segundos também são descartados. Recarregar a página, trocar a origem entre
real/simulado ou receber um percurso menor que o anterior reinicia o cálculo.
Ele representa os trechos acompanhados nesta página, não o histórico inteiro
do veículo nem uma previsão de autonomia calculada pelo navegador.

## Manutenção e fontes de verdade

- Ajuste rede e UART nos respectivos `include/nucleo/configuracao.h`; use o
  contrato único `firmware/compartilhado/protocolos/configuracao_enlace_uart.h`
  nos três firmwares.
- Edite HTML/CSS/JS em `web/` e regenere `painel_web.c` antes da compilação.
- Ao mudar campos binários, atualize o protocolo compartilhado e a versão de
  telemetria. Ao mudar a API, revise `servidor_web.c` e o JavaScript do painel.
- Os detalhes de enquadramento estão no [guia de protocolos](protocolos.md)
  e a atualização de firmware no [guia OTA](ota.md).

## Controle de marcha e acelerador

A aba mostra marcha física, marcha permitida e sentido atual. O operador
pode limitar a marcha máxima a L/M/H, permitir ou bloquear a ré, ajustar
velocidade máxima, teto do acelerador e zona morta. Velocidade máxima e teto
do pedal são persistidos na NVS. A tensão mínima do DAC tem padrão na
configuração do Mestre. O pedal físico é lido no GPIO5, e a aba identifica
quando o limite de velocidade está modulando a referência.

**Parada de emergência** envia o pedido ao pressionar. O Mestre prioriza o
quadro recebido, e a vaga de ações HTTP compartilhada pela Equipe pode
devolver 503 quando já estiver ocupada. **Rearmar** exige confirmação no
painel após 3 s. O Mestre valida separadamente a espera de 5 s da parada
atual e a validade de 2 s a partir da primeira recepção UART. A aba mostra
o estado lógico e a situação da atuação física. Consulte [Controle de marcha
e acelerador](controle-remoto.md) para os intertravamentos, os caminhos de
rearme e o contrato.

Em **Controle**, a ré começa bloqueada. **Permitir ré** libera pedidos de ré
que passem pelo Mestre, e **Bloquear ré** provoca uma parada se houver uma ré
solicitada. A confirmação dessas mudanças e do rearme manual libera o botão
após 3 s.
