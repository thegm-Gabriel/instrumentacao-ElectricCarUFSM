# Guia técnico dos satélites e das APIs locais

[Índice da documentação](README.md)

Este guia descreve o código atual de `firmware/equipe-s3` e
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
   Só publica um retrato depois de conferir início, versão, tipo, tamanho,
   CRC-16 e plausibilidade dos campos. Registra CRC, cabeçalho, conteúdo,
   duplicatas, perdas e reinícios do mestre. Um contador de tempo do mestre
   retrocedendo provoca resincronização da sequência.
3. Cada pacote aceito produz um ACK de 20 bytes com sequência, tempo do mestre,
   identidade, versão e capacidades do satélite. O mestre usa esse retorno para
   acompanhar a saúde dos enlaces. Quadros de comandos e OTA também têm respostas
   pelo mesmo TX do satélite.
4. O retrato da API permanece disponível depois de uma interrupção do enlace.
   Consulte `valido` e `idade_ms` para distinguir ausência inicial de dados e
   uma amostra antiga; a interface sinaliza desatualização.

Ao iniciar, ambos confirmam uma imagem OTA pendente somente após UART, Wi-Fi e
HTTP funcionarem. Se essa inicialização falhar, solicitam rollback. A equipe
inicializa diagnóstico local como recurso complementar.

## ESP32-S3 da equipe

`include/nucleo/configuracao.h` define o AP `UFSM-Carro-Equipe`, canal 11,
até cinco clientes e a UART1 em GPIO 5 (RX) e 6 (TX). A rede padrão do ESP-IDF
usa `192.168.4.1` e DHCP. O firmware anuncia o portal HTTP pela opção DHCP 114,
quando suportada. Ele não oferece servidor DNS local; acesso confiável:
`http://192.168.4.1/`.

`web/index.html`, `web/style.css`, `web/layout.css` e `web/app.js` são as fontes
do painel NEXUS. `web/gerar_painel.py` gera `src/servicos/painel_web.c`,
incorporado ao binário. As rotas `/`, `/painel`, `/diagnostico` e `/mestre`
entregam a mesma página com três abas: mestre, equipe e visitantes. O painel
atualiza telemetria, diagnóstico e respostas de consultas. Gráficos e CSV do
navegador acumulam dados apenas enquanto a página está aberta.

| Método e rota | Função |
|---|---|
| `GET /api/telemetria` | Última amostra do veículo, idade, sequência e métricas de recepção UART. |
| `GET /api/diagnostico` | Versão e estado da equipe, Wi-Fi, heap/PSRAM, CPU, tarefas, temperatura, contadores UART, comandos e OTA. |
| `GET /api/mestre?consulta=...` | Consulta o mestre pela UART e transforma a resposta em JSON. |
| `POST /api/mestre?consulta=...` | Solicita uma ação ao mestre; a resposta confirma o recebimento/aceitação, e ações assíncronas exigem consulta posterior do estado. |

Consultas GET aceitas: `ping`, `versao`, `capacidades`, `saude`, `telemetria`,
`uart`, `ota`, `wifi`, `diagnostico_mestre`, `configuracao`,
`rede_wifi_salva` (com `indice` de 1 a 5), `satelite_equipe_ping`,
`satelite_equipe_versao`, `satelite_visitantes_ping` e
`satelite_visitantes_versao`.

Ações POST aceitas: `verificar_ota`, `autorizar_mestre`, `autorizar_equipe`,
`autorizar_visitantes`, `autorizar_todos`, `cancelar_ota`, `resumo_terminal`,
`alternar_relatorios`, `zerar_percurso`, `sincronizar_google`,
`intervalo_relatorio`, `intervalo_ota`, `intervalo_telemetria_ota`,
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
| `wifi_preparar` | SSID, quebra de linha e senha da rede. Prepara a próxima operação. |
| `wifi_conectar_preparada`, `wifi_salvar_preparada` | Vazio; exigem uma rede preparada. |
| `wifi_conectar_salva`, `wifi_excluir_salva` | Índice de 1 a 5, conforme a lista consultada. |
| `wifi_automatico` | `0` ou `1`, quebra de linha e senha de permissão. |
| `wifi_alterar_principal`, `wifi_restaurar_principal` | Senha de permissão; a alteração exige rede previamente preparada. |
| `wifi_politica` | Cinco linhas: tentativas por rede, tempo por tentativa em ms, intervalo entre tentativas em ms, limite da sessão em ms e senha de permissão. |

O HTTP rejeita consulta ausente ou desconhecida e método incompatível com
`400` ou `405`. Consultas e ações são encaminhadas pela UART; conexão a uma
rede Wi-Fi pode aguardar até cerca de 185 s, enquanto consultas de satélite
usam prazo menor. A confirmação visual antes do envio pertence ao JavaScript
do painel. A API HTTP não exige token próprio: um cliente conectado ao AP pode
enviar ações diretamente, sujeitas às validações e às senhas exigidas pelo
serviço do mestre. `ok:true` indica que a resposta de comando foi aceita;
para OTA, upload e outras ações assíncronas, consulte o estado depois.

O painel não recupera sessões passadas do SD nem confirma sozinho o efeito final
de uma operação assíncrona. Tensão do pacote, corrente, carga e pedais não são
adquiridos no modo real atual; esses valores só são exibidos quando a telemetria
indica modo simulado. `tensao_adc_bruta` é apenas a contagem não calibrada do ADC.

## ESP32-S3 de visitantes

`include/nucleo/configuracao.h` define o AP `UFSM-Carro-Visitantes`, canal 1,
até dez clientes, largura solicitada de 20 MHz e potência máxima solicitada de
16 dBm, além da UART1 em GPIO 5 (RX) e 6 (TX). Canal, largura e potência podem
ser alterados por flags de compilação dentro dos limites do cabeçalho. O driver
registra os valores efetivamente aplicados. O AP usa `192.168.4.1`, DHCP e DNS
cativo local opcional: quando ativo, responde consultas IPv4 com o IP do painel.
O acesso por IP continua funcionando se a tarefa DNS não iniciar.

`web/index.html`, `web/style.css` e `web/app.js` são compilados em
`src/servicos/painel_web.c` por `web/gerar_painel.py`. Há duas rotas:
`GET /` para a página pública e `GET /api/telemetria` para o JSON. Não há API
de diagnósticos ou de comandos nessa rede. O endpoint publica validade, idade,
sequência, origem simulada, velocidade, aceleração, potência e percurso, com
unidades codificadas nos nomes dos campos. Aceleração indisponível ou grandezas
elétricas não adquiridas são `null`, não zero. Os gráficos e cálculos de energia,
distância e tempo começam quando a página é aberta.

## Manutenção e fontes de verdade

- Ajuste rede e UART nos respectivos `include/nucleo/configuracao.h`; mantenha
  `configuracao_enlace_uart.h` igual nos três firmwares.
- Edite HTML/CSS/JS em `web/` e regenere `painel_web.c` antes da compilação.
- Ao mudar campos binários, atualize o protocolo compartilhado e a versão de
  telemetria. Ao mudar a API, revise `servidor_web.c` e o JavaScript do painel.
- Os detalhes de enquadramento estão no [guia de protocolos](protocolos.md)
  e a atualização de firmware no [guia OTA](ota.md).
