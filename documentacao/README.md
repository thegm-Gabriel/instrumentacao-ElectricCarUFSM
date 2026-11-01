# Instrumentação do carro elétrico — documentação

Este é o ponto de entrada da documentação do sistema de instrumentação e
telemetria do **Carro Elétrico UFSM-CS**. O projeto tem um
ESP32-S3 mestre para aquisição, diagnóstico, registro e atualização, além de
dois ESP32-S3 satélites que apresentam os dados à equipe e aos visitantes.
Os guias descrevem o comportamento implementado, o modo de operação e os
limites conhecidos. O código e os cabeçalhos de configuração são a referência
final para constantes e contratos binários. Leia primeiro a visão geral e
depois o guia do componente que pretende operar ou modificar.

## Comece por aqui

| Assunto | Guia | O que contém |
|---|---|---|
| Como o sistema se divide | [Arquitetura](arquitetura.md) | Fluxos, camadas, inicialização, tarefas, supervisão e recuperação. |
| Por que o código foi dividido assim | [Filosofia e fluxos](filosofia.md) | Responsabilidade de cada camada, sequência de leitura/comando/falha e regras para modificar o sistema. |
| Sensores e ligações | [Pinagem](pinagem.md) | Todos os GPIOs, alimentação, níveis lógicos e barramentos. |
| Controlador principal | [Firmware mestre](mestre.md) | Aquisição, odômetro, telemetria, SD, terminal, Wi-Fi, diagnóstico, OLED e sinalização. |
| Painéis locais | [Satélites e APIs](satelites.md) | Equipe, visitantes, rotas HTTP, comandos e comportamento sem enlace. |
| Formato dos dados | [Protocolos](protocolos.md) | Pacotes UART de telemetria, ACK, comandos e OTA. |
| Atualizações | [OTA e releases](ota.md) | Manifesto, primeira gravação, autorização, publicação e rollback. |
| Números de versão | [Versionamento](versionamento.md) | Major, minor, patch, versões independentes e preparação/publicação. |
| Comandos de desenvolvimento | [Ferramentas](ferramentas.md) | Geradores das páginas e QR, segredos, compilação, testes e scripts de release. |
| Histórico remoto | [Google Drive e Sheets](google.md) | CSV, Apps Script, credenciais, envio e recuperação. |

## Visão do sistema

```text
sensores, encoders ──► ESP32-S3 mestre ◄──UART──► satélite equipe ──Wi-Fi──► painel
                           │          ◄──UART──► satélite visitantes ─Wi-Fi─► painel
                           ├──SPI2──► microSD (CSV e logs)
                           ├──I²C───► MPU6050 e OLED
                           └──Wi-Fi temporário──► releases OTA e Google
```

O mestre publica telemetria normal a 4 Hz. Cada satélite valida o quadro,
mantém o último retrato e devolve um ACK. A equipe também pode consultar e
solicitar ações ao mestre pela UART; o painel público de visitantes expõe
somente dados de telemetria e informações sobre o projeto. O mestre registra
CSV a 1 Hz no cartão e envia os arquivos pendentes ao Google quando solicitado.
O cartão também contém diagnóstico de baixa cadência e capturas detalhadas após
falhas. Atualizações OTA podem atingir cada firmware separadamente ou todos em
sequência, com os satélites primeiro. Um comando aceito não é, por si, uma
operação concluída: o painel acompanha o estado posterior de OTA e Google.

| Informação | Onde nasce | Onde é vista | Persistência |
|---|---|---|---|
| Velocidade, aceleração e percurso | Sensores/odômetro ou simulador no mestre | OLED, terminal e painéis via UART | Amostra ao vivo; cópia CSV no SD a 1 Hz. |
| Estado dos ESPs e dos enlaces | Diagnóstico local e respostas UART | Terminal e painel técnico da Equipe | Resumos e incidentes do mestre no log SD. |
| Histórico dos gráficos | Amostras recebidas pelo navegador | Página aberta da Equipe ou de Visitantes | Somente na sessão do navegador; não recupera CSV do SD. |
| Arquivos pendentes para Google | Registro CSV do mestre | Terminal e página Comandos | Permanecem no SD até confirmação do Web App. |
| Estado de OTA | Serviço OTA e respostas dos satélites | Terminal, OLED e página Comandos | Versão instalada no firmware; estado de progresso da operação é transitório. |

## Onde está o código

| Caminho | Responsabilidade |
|---|---|
| [`firmware/mestre-s3`](../firmware/mestre-s3/) | Aquisição, diagnóstico, odômetro, SD, Wi-Fi, OTA e terminal. |
| [`firmware/equipe-s3`](../firmware/equipe-s3/) | Recepção UART, painel técnico e comandos remotos. |
| [`firmware/visitantes-s3`](../firmware/visitantes-s3/) | Recepção UART e painel público. |
| [`firmware/compartilhado`](../firmware/compartilhado/) | Protocolos, modelos e serviços usados por vários firmwares. |
| [`hardware`](../hardware/) | Projetos de placa e referências físicas. |
| [`ferramentas`](../ferramentas/) | Scripts de segredos, QR, testes, manifestos e releases; veja o [manual](ferramentas.md). |
| [`versoes`](../versoes/) | Estado do último release preparado; `saida` e `historico` guardam artefatos locais de publicação. |

Cada pasta de `firmware` é um projeto PlatformIO com ESP-IDF. O mestre usa
ESP32-S3-WROOM-1-N16R8, com 16 MB de flash e 8 MB de PSRAM; cada satélite usa
4 MB de flash com duas partições OTA de aplicação.
Os arquivos sob `hardware` registram referências físicas; confira o conteúdo
disponível antes de tomar um esquema como revisão validada da montagem.

## Compilar e gravar pela primeira vez

Instale Python e PlatformIO. Na raiz do repositório, execute um alvo por vez:

```powershell
pio run -d firmware\mestre-s3 -e mestre-s3-n16r8
pio run -d firmware\equipe-s3 -e esp32-s3-devkitm-1
pio run -d firmware\visitantes-s3 -e esp32-s3-devkitm-1
```

Para gravar por cabo, acrescente `-t upload` ao comando do alvo. A primeira
gravação instala a tabela com duas partições OTA; depois, consulte o
[procedimento de atualização](ota.md). Saídas `.pio`, binários, caches e
configurações pessoais não devem ser adicionados ao Git.

Antes da montagem, confira a [pinagem](pinagem.md), os níveis de 3,3 V e o GND
comum. Copie
`firmware/mestre-s3/include/nucleo/segredos_ota.exemplo.h` para
`segredos_ota.h` e preencha só a cópia local. Revise geometria, orientação e
limites em `firmware/mestre-s3/include/nucleo/configuracao_placa.h`.
Configure o Google apenas se o upload remoto for utilizado. O manual de
[ferramentas](ferramentas.md) mostra como gerar as páginas incorporadas e
o cabeçalho local de credenciais.

## Acesso aos painéis

- Na rede `UFSM-Carro-Equipe`, abra `http://192.168.4.1/` por HTTP. As rotas
  `/painel`, `/diagnostico` e `/mestre` entregam a mesma interface com seis
  páginas: **Telemetria, Comandos, ESP32 — Mestre, ESP32 — Equipe,
  ESP32 — Visitantes e Sobre**. A página Comandos contém ações, clientes Wi-Fi,
  OTA e Google; as páginas dos ESPs apresentam diagnóstico. O AP da Equipe não
  oferece DNS local; o anúncio do portal pelo DHCP depende do cliente.
- Na rede `UFSM-Carro-Visitantes`, abra `http://192.168.4.1/`. O DNS cativo
  responde consultas IPv4 com esse IP quando a tarefa DNS está disponível. A
  página pública apresenta telemetria, histórico da sessão e Sobre, sem
  controles remotos do mestre.

Cada `192.168.4.1` pertence ao AP conectado naquele momento. As APIs e as
limitações dos dados reais e simulados estão no [guia dos satélites](satelites.md).

## Uso durante um teste

1. Ligue o mestre e os satélites com GND e enlaces UART conforme
   [pinagem](pinagem.md). Observe no terminal do mestre (115200 baud) se a
   partida informou falha ou operação degradada.
2. Conecte o computador à rede da Equipe e abra o painel pelo IP acima. Em
   **Telemetria**, confira origem real/simulada, idade da amostra e estado dos
   enlaces antes de interpretar um número. Uma leitura antiga não representa o
   instante atual.
3. Use **Comandos** para verificar Google/OTA, alterar a fonte simulada em
   testes e administrar as redes. Acompanhe resposta e estado posterior; a
   aceitação da solicitação não encerra uma operação em segundo plano.
4. Após o teste, consulte os CSVs e logs do SD. O CSV preserva amostras de
   percurso; o log traz resumos e contexto de incidentes. Para sincronizar o
   histórico remoto, solicite Google pelo terminal ou pela página Comandos e
   aguarde a confirmação. Veja [mestre](mestre.md) e [Google](google.md).
5. Para atualizar firmware, prepare as imagens/manifesto, autorize o alvo e
   acompanhe versão, progresso e rollback conforme [OTA](ota.md). Não use a
   compilação comum como substituto do conjunto preparado para publicação.

## Ao modificar o projeto

Atualize o guia do assunto junto com o código. Mudanças de GPIO ou eletrônica
exigem revisão de [pinagem](pinagem.md); mudanças no formato UART exigem revisão
dos [protocolos](protocolos.md) e dos três firmwares. As fontes HTML/CSS/JS dos
painéis ficam em `firmware/*/web/` e devem gerar novamente `painel_web.c` antes
de compilar. Consulte [filosofia e fluxos](filosofia.md#ao-alterar-o-código)
para a lista de verificações por tipo de mudança. O
[teste de protocolo](../firmware/mestre-s3/test/test_protocolo_telemetria/)
cobre serialização, CRC, ACK, sequência e recuperação do parser; a validação
elétrica e funcional no veículo ainda depende de bancada.
