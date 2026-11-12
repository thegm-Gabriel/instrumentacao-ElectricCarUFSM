# Guia técnico dos satélites e das APIs locais

[Índice da documentação](README.md)

Este guia descreve os dois satélites do NEXUS, em `firmware/equipe-s3` e
`firmware/visitantes-s3`. Ambos são projetos PlatformIO/ESP-IDF independentes,
com partições OTA em flash de 4 MB. Os contratos de telemetria, comandos e OTA
ficam em `firmware/compartilhado`.

Em ambos, `src/main.c` inicializa os módulos; `gerenciadores/gerenciador_wifi.c`
cuida do AP, `servicos/receptor_uart.c` recebe os três tipos de quadro e
`servicos/servidor_web.c` publica HTTP. A equipe tem ainda
`gerenciadores/gerenciador_uart.c` para serialização e estatísticas do enlace e
`servicos/diagnostico_sistema.c` para CPU, memória e temperatura. Os serviços
`firmware/compartilhado/comandos/servico_comandos_satelite.c` e
`firmware/compartilhado/ota/servico_ota_satelite.c` implementam respostas de
comandos e gravação OTA para os dois satélites.

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

## ESP32-S3 da equipe

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
entregam o mesmo painel com sete páginas: telemetria, comandos, controle,
mestre, equipe, visitantes e sobre.

| Página da Equipe | O que apresenta ou permite fazer |
|---|---|
| Telemetria | Velocidade, movimento, energia disponível, histórico desta sessão, qualidade do enlace e contagem de clientes dos APs. |
| Comandos | Envio e acompanhamento de ações; Google, simulação, Wi-Fi externo do mestre, clientes dos dois APs e OTA. |
| Controle | Telemetria essencial, marcha e limites solicitados, liberação e parada remota de envio direto. Não há atuação física nesta versão. |
| ESP32 — Mestre | Saúde, causas de degradação, memória, CPU, tarefas e estado obtido por consultas UART. |
| ESP32 — Equipe | Diagnóstico local, recepção UART e estado do ponto de acesso técnico. |
| ESP32 — Visitantes | Diagnóstico remoto, disponibilidade, memória, CPU e clientes quando o satélite responde. |
| Sobre | Intenção do projeto, símbolo, orientação e equipe discente. |

As páginas dos ESPs mostram diagnóstico; ações de rede, Google e OTA ficam em
Comandos, enquanto pedidos de condução ficam em Controle. CPU em uso, memória interna livre e temperatura aparecem em destaque
nos três diagnósticos. As abas e os indicadores de conexão se reorganizam em
linhas no celular. Na página Comandos, os botões preenchidos identificam ações;
consultas de estado têm apresentação secundária. Os atalhos abrem a seção
correspondente, inclusive quando recolhida, e os ajustes protegidos de Wi-Fi
têm um campo próprio para permissão administrativa.
O resumo de saúde do mestre é consultado periodicamente:
uma operação degradada ou falha aparece em aviso discreto com suas causas ativas
e acesso ao diagnóstico. A telemetria pode ser consultada a 1, 2, 3 ou 4 Hz
(padrão de 4 Hz, compatível com os 250 ms da publicação do mestre). As consultas
de estado OTA, Google e simulador continuam automáticas a cada 2 s,
independentemente dessa escolha. O cartão Google também permite iniciar a
sincronização dos arquivos pendentes e acompanhar o progresso informado pelo
mestre. Uma requisição lenta ou falha pode reduzir a taxa efetiva no navegador.
Gráficos e CSV do navegador acumulam dados apenas enquanto a página está aberta.
Os gráficos combinam séries no mesmo intervalo de tempo, com duas escalas e
unidades nas laterais. Movimento mostra velocidade (escala inicial de 0 a 30 km/h,
expandida quando uma amostra excede esse valor e restaurada depois)
e aceleração; o gráfico elétrico permite escolher duas grandezas com eixos
laterais e uma terceira curva opcional, tracejada, com escala própria indicada
na legenda. Passar o mouse ou tocar no gráfico mostra os valores da amostra. Os
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
10 km/h. O pico também aparece brevemente no título do cartão. O número ganha
tom âmbar durante uma queda perceptível de velocidade; isso representa apenas
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

Os cartões de aceleração e potência distinguem desaceleração e regeneração com
cor e texto. Quando há potência, uma faixa bipolar põe zero no centro, consumo
à direita e regeneração à esquerda; começa em ±500 W e cresce em passos de
250 W se necessário. Um risco discreto marca por 1,6 s um pico de magnitude
de pelo menos 120 W seguido de redução de pelo menos 55 W ou 25%, com 3,5 s
entre marcas. O cartão mantém “Medição indisponível” quando o modo real não
fornece potência. Os cartões e o fundo usam cores suaves para separar movimento,
energia e diagnósticos. A navegação usa uma linha de seleção e permanece
rolável horizontalmente em telas estreitas.

A página Sobre explica o projeto Carro Elétrico UFSM-CS, identifica orientação e equipe discente
e apresenta o símbolo do carro. O painel público de Visitantes também traz essa
identidade e os créditos. A imagem fonte fica em
`firmware/compartilhado/web/logo_carro_eletrico.png` e é incorporada ao HTML
pelos dois geradores, sem requisição adicional no navegador.

Para experimentar a interface no PC ou celular sem gravar o ESP, use
`python ferramentas/visualizar_painel_equipe.py --rede`. A prévia possui
telemetria, comandos e cenários de saúde simulados; veja o procedimento e as
limitações em [ferramentas](ferramentas.md#prévia-local-da-página-da-equipe).

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
senha da rede é o controle de acesso aos comandos. A permissão de certas
alterações de Wi-Fi no mestre permanece separada.

Consultas GET aceitas: `ping`, `versao`, `saude`, `telemetria`,
`uart`, `ota`, `sincronizacao`, `wifi`, `diagnostico_mestre`, `configuracao`,
`simulador`,
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
`wifi_alterar_principal`, `wifi_restaurar_principal` e `wifi_politica`.
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
| `wifi_politica` | Cinco linhas: tentativas por rede, tempo por tentativa em ms, intervalo entre tentativas em ms, limite da sessão em ms e senha de permissão. |

O HTTP rejeita consulta ausente ou desconhecida e método incompatível com
`400` ou `405`. Consultas e ações aceitas são
encaminhadas pela UART; conexão a uma rede Wi-Fi pode aguardar até cerca de
185 s, enquanto consultas de satélite usam prazo menor. A confirmação visual
antes do envio pertence ao JavaScript do painel. `ok:true` indica que a
resposta de comando foi aceita;
para OTA, upload e outras ações assíncronas, consulte o estado depois.

O painel não recupera sessões passadas do SD nem confirma sozinho o efeito final
de uma operação assíncrona. Tensão do barramento, corrente e potência vêm do
INA228 quando o mestre marca a leitura elétrica como válida. A página da equipe
mostra também, dentro de **Operação e sensores**, queda no shunt, temperatura
do chip, energia e carga acumuladas desde o reset do INA228 e diagnóstico
quando disponíveis. Não há um cartão separado para o sensor; carga percentual,
autonomia e pedais ainda só são apresentados no modo simulado.
`tensao_adc_bruta` é apenas a contagem não calibrada do ADC.

## ESP32-S3 de visitantes

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

`web/index.html`, `web/style.css` e `web/app.js` são compilados em
`src/servicos/painel_web.c` por `web/gerar_painel.py`. Há duas rotas:
`GET /` para a página pública e `GET /api/telemetria` para o JSON. Não há API
de diagnósticos ou de comandos nessa rede. O endpoint publica validade, idade,
sequência, origem simulada, velocidade, aceleração, percentuais de acelerador e
freio, potência e percurso, com
unidades codificadas nos nomes dos campos. Aceleração indisponível ou grandezas
elétricas não adquiridas são `null`, não zero. Os gráficos e cálculos de energia,
distância e tempo começam quando a página é aberta.

### Leitura e apresentação pública

O cabeçalho identifica **NEXUS** como sistema de telemetria e controle e
aponta para o Sobre no fim da página. O painel público só lê dados: não expõe
os comandos do sistema. A página apresenta velocidade, aceleração, percurso e,
quando recebidos, bateria e grandezas elétricas. No modo real, os campos ainda
sem aquisição aparecem como “Sem leitura” ou com uma nota de indisponibilidade;
zero permanece uma leitura válida. Os botões “i” nos cartões de acompanhamento
abrem descrições curtas por toque, clique ou teclado.

O velocímetro usa **escala fixa de 0 a 35 km/h** e marcações de 5 em 5 km/h;
10, 20 e 30 têm maior destaque. O preenchimento percorre o mesmo arco de verde
para amarelo, laranja e vermelho conforme a posição, sem recolorir toda a
parte já preenchida. Se a leitura nova cair pelo menos 0,18 km/h, o número
recebe um tom âmbar temporário; ele volta ao branco ao estabilizar, acelerar ou
parar. Essa cor descreve uma tendência entre amostras, não uma condição de
segurança. “Sem leitura” usa fonte menor para caber dentro do arco.

Após um pico recente de pelo menos 5 km/h, uma queda de 1,4 km/h ou mais a
uma taxa de pelo menos 2,3 km/h por segundo mostra um risco discreto **dentro**
do arco, na posição do pico, e o texto “Pico”. O texto começa a desaparecer
após cerca de 1,05 s e o risco termina após 1,6 s, ambos com saída gradual.
Há intervalo de 3,8 s entre eventos, e o marcador só é rearmado depois de
nova subida de pelo menos 2 km/h acima do vale. Amostras antigas ou inválidas
reiniciam o rastreamento. O marcador representa uma redução observada no
navegador; não informa a velocidade máxima configurada para o veículo.

As barras de acelerador e freio crescem da extremidade externa para o centro,
respectivamente da esquerda para a direita e da direita para a esquerda.
`acelerador_decimos_percentual` e `freio_decimos_percentual` usam décimos de
por cento. Como o firmware atual só obtém esses valores na simulação, no modo
real são `null`. Aceleração negativa colore seu cartão em coral suave; o cartão
de potência distingue consumo de regeneração com tons próprios. Rótulo, cor e
número acompanham a mesma transição ao atravessar zero.

Quando há potência, o cartão principal mostra uma faixa com **zero no centro**:
consumo cresce à direita e regeneração, à esquerda. A escala começa em ±500 W
e só aumenta durante a sessão se a potência recebida exceder esse alcance;
não oscila para baixo a cada amostra. Um risco sem rótulo marca, por cerca de
1,55 s, um pico de magnitude de pelo menos 120 W seguido de queda de pelo menos
55 W ou 25% (o maior dos dois). Uma inversão de sinal também pode marcar o
pico anterior; há intervalo de 3,5 s entre marcas. A faixa e esse cartão ficam
ocultos quando não há leitura elétrica.

O mestre publica telemetria aos satélites nominalmente a cada 250 ms (4 Hz);
o navegador de Visitantes consulta `/api/telemetria` a cada 500 ms (2 Hz), sem
ajuste de taxa. Ele aceita apenas sequências novas e estima o período de chegada
dessas leituras. Números e barras se aproximam do alvo com desaceleração no fim:
a transição usa cerca de 1,15 vez esse período, limitada a 450–850 ms. Uma
leitura nova interrompe a transição a partir do valor exibido. Movimento
reduzido solicitado pelo navegador ou aba oculta aplica o valor imediatamente.
O tempo de acompanhamento continua avançando como relógio. Mais de 2,5 s sem
leitura nova abre uma lacuna nos gráficos.

O gráfico **Movimento** une velocidade e aceleração na mesma linha de tempo,
com escala e unidade próprias nas bordas esquerda e direita. A velocidade
começa em 0–30 km/h e expande o eixo quando as leituras visíveis exigem; isso
não altera a escala fixa do velocímetro. O gráfico **Sistema elétrico** permite
selecionar potência, corrente, tensão, estado de carga e autonomia simulados ou,
quando disponíveis, queda de tensão no shunt, temperatura, energia e carga
acumuladas do INA228. As duas primeiras curvas usam os eixos laterais; a
terceira é opcional, tracejada e usa escala própria indicada na legenda. O toque
ou ponteiro mostra o valor de cada grandeza na amostra. Ambos usam janelas de
30 segundos, 2 ou 10 minutos e preservam
lacunas em vez de ligar pontos através de falhas. O histórico existe apenas
enquanto a página está aberta.

O Sobre traz o símbolo do carro, a intenção do projeto, orientação e equipe
discente. A tipografia dos cartões largos cresce no computador sem ampliar os
cartões estreitos do celular; não há fontes ou bibliotecas carregadas da rede.

A seção de energia mostra tensão com três casas decimais e os demais valores
disponíveis com pelo menos duas casas decimais, sem alterar a precisão da
medição original. Também apresenta **autonomia restante** em km, recebida no
campo `autonomia_decimos_km`, e **consumo médio** em Wh/km. A autonomia é uma
estimativa do mestre; atualmente só existe no modo simulado e é `null` no modo
real. A prévia local fornece esse campo para experimentar a interface.
O consumo médio é calculado pelo navegador, dividindo a energia positiva
integrada pela distância dos mesmos intervalos válidos. Inclui energia gasta
parado e não desconta regeneração. Só aparece após 100 m de distância acumulada
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

## Preparação de controle remoto

Na página **Comandos**, Última ação, Google, simulação, Wi-Fi do mestre,
redes locais, consultas, OTA, operação e histórico começam recolhidos. Os
atalhos no topo abrem automaticamente a seção escolhida e levam o foco até
ela. Subseções internas podem ser abertas separadamente. A página **Controle**
reúne telemetria recente, marcha, limites e pedido de parada. Esse pedido é
enviado sem diálogo de confirmação, com aviso discreto sobre a ausência de
atuação física; os demais pedidos mantêm a confirmação.

A Equipe pode solicitar marcha, limites de velocidade e aceleração e parada
remota ao mestre. O estado é apenas preparatório, sem atuação física. Consulte
[Controle remoto](controle-remoto.md) para o contrato UART/HTTP, intertravamentos,
reinício, acompanhamento no painel e requisitos da integração futura.
