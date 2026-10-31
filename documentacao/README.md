# Instrumentação do carro elétrico — documentação

Este é o ponto de entrada da documentação do sistema de instrumentação e
telemetria do carro elétrico de pequeno porte da UFSM-CS. O projeto tem um
ESP32-S3 mestre para aquisição, diagnóstico, registro e atualização, além de
dois ESP32-S3 satélites que apresentam os dados à equipe e aos visitantes.
Todos os guias de funcionamento estão nesta pasta; o código e os cabeçalhos
de configuração são a referência final para constantes e contratos binários.

## Comece por aqui

| Assunto | Guia | O que contém |
|---|---|---|
| Como o sistema se divide | [Arquitetura](arquitetura.md) | Fluxos, camadas, inicialização, tarefas, supervisão e recuperação. |
| Sensores e ligações | [Pinagem](pinagem.md) | Todos os GPIOs, alimentação, níveis lógicos e barramentos. |
| Controlador principal | [Firmware mestre](mestre.md) | Aquisição, odômetro, telemetria, SD, terminal, Wi-Fi, diagnóstico, OLED e sinalização. |
| Painéis locais | [Satélites e APIs](satelites.md) | Equipe, visitantes, rotas HTTP, comandos e comportamento sem enlace. |
| Formato dos dados | [Protocolos](protocolos.md) | Pacotes UART de telemetria, ACK, comandos e OTA. |
| Atualizações | [OTA e releases](ota.md) | Manifesto, primeira gravação, autorização, publicação e rollback. |
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
somente dados de telemetria. O mestre registra CSV a 1 Hz no cartão e envia os
arquivos pendentes ao Google quando solicitado. Atualizações OTA podem atingir
cada firmware separadamente ou todos em sequência, com os satélites primeiro.

## Onde está o código

| Caminho | Responsabilidade |
|---|---|
| [`firmware/mestre-s3`](../firmware/mestre-s3/) | Aquisição, diagnóstico, odômetro, SD, Wi-Fi, OTA e terminal. |
| [`firmware/equipe-s3`](../firmware/equipe-s3/) | Recepção UART, painel técnico e comandos remotos. |
| [`firmware/visitantes-s3`](../firmware/visitantes-s3/) | Recepção UART e painel público. |
| [`firmware/compartilhado`](../firmware/compartilhado/) | Protocolos, modelos e serviços usados por vários firmwares. |
| [`hardware`](../hardware/) | Projetos de placa e referências físicas. |
| [`ferramentas`](../ferramentas/) | Preparação de segredos, manifestos e releases. |
| [`versoes`](../versoes/) | Estado versionado e exemplos de manifestos OTA. |

Cada pasta de `firmware` é um projeto PlatformIO com ESP-IDF. O mestre usa
ESP32-S3-WROOM-1-N16R8, com 16 MB de flash e 8 MB de PSRAM; cada satélite usa
4 MB de flash com duas partições OTA de aplicação.
`hardware/kicad` está reservado para esquemas e placas; `hardware/folhas-de-dados`
está reservado para referências dos componentes, com origem e revisão ao
adicionar cada arquivo.

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
Configure o Google apenas se o upload remoto for utilizado.

## Acesso aos painéis

- Na rede `UFSM-Carro-Equipe`, abra `http://192.168.4.1/` por HTTP. As rotas
  `/painel`, `/diagnostico` e `/mestre` entregam a mesma interface com três
  abas. O AP da equipe não oferece DNS local; o anúncio do portal pelo DHCP
  depende do cliente.
- Na rede `UFSM-Carro-Visitantes`, abra `http://192.168.4.1/`. O DNS cativo
  responde consultas IPv4 com esse IP quando a tarefa DNS está disponível.

Cada `192.168.4.1` pertence ao AP conectado naquele momento. As APIs e as
limitações dos dados reais e simulados estão no [guia dos satélites](satelites.md).

## Ao modificar o projeto

Atualize o guia do assunto junto com o código. Mudanças de GPIO ou eletrônica
exigem revisão de [pinagem](pinagem.md); mudanças no formato UART exigem revisão
dos [protocolos](protocolos.md) e dos três firmwares. As fontes HTML/CSS/JS dos
painéis ficam em `firmware/*/web/` e devem gerar novamente `painel_web.c` antes
de compilar. O [teste de protocolo](../firmware/mestre-s3/test/test_protocolo_telemetria/)
cobre serialização, CRC, ACK, sequência e recuperação do parser; a validação
elétrica e funcional no veículo ainda depende de bancada.
