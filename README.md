# Instrumentação do carro elétrico

Firmware e documentação do sistema de instrumentação e telemetria do carro
elétrico de pequeno porte da UFSM-CS. O projeto usa um ESP32-S3 mestre para
aquisição, diagnóstico, registro e atualização, além de dois ESP32-S3 satélites
que apresentam os dados à equipe e aos visitantes.

## Visão geral

```text
Sensores e encoders
        │
        ▼
ESP32-S3 mestre ◄─ UART ─► ESP32-S3 da equipe ── Wi-Fi ──► painel em 192.168.4.1
        ├────────── UART ◄─► ESP32-S3 de visitantes ─► painel em 192.168.4.1
        ├────────── microSD ──► CSVs a 1 Hz
        └────────── Wi-Fi ────► Google Drive/Sheets e releases OTA
```

Principais recursos:

- telemetria ao vivo a 4 Hz, com enquadramento binário e CRC-16;
- MPU6050, medição de tensão, dois encoders e odômetro persistente;
- diagnóstico de sensores, barramentos, tarefas, memória e enlace UART;
- avisos alternados na tela OLED e sinalização por LEDs e buzzer;
- painéis web locais, responsivos e sem dependências externas;
- registro histórico em CSV no cartão SD a 1 Hz e envio manual ao Google;
- OTA individual ou coordenada, atualizando os satélites antes do mestre;
- comandos remotos do mestre pela página exclusiva da equipe.

## Organização do repositório

| Caminho | Responsabilidade |
|---|---|
| [`firmware/mestre-s3`](firmware/mestre-s3/) | Aquisição, processamento, diagnóstico, SD, Google, terminal e orquestração OTA. |
| [`firmware/equipe-s3`](firmware/equipe-s3/) | Recepção UART e painéis locais da equipe, diagnóstico e mestre. |
| [`firmware/visitantes-s3`](firmware/visitantes-s3/) | Recepção UART e painel simplificado para visitantes. |
| [`firmware/compartilhado`](firmware/compartilhado/) | Protocolos e modelos usados por mais de um firmware. |
| [`hardware`](hardware/) | Pinagem, projetos KiCad e referências dos componentes. |
| [`documentacao`](documentacao/) | Arquitetura, OTA e integração da telemetria com o Google. |
| [`ferramentas`](ferramentas/) | Scripts para segredos, manifestos e preparação de releases. |
| [`versoes`](versoes/) | Estado versionado e exemplos dos manifestos OTA. |

Cada pasta em `firmware` é um projeto PlatformIO independente baseado em
ESP-IDF. O mestre usa o ESP32-S3-WROOM-1-N16R8, com 16 MB de flash e 8 MB de
PSRAM; os dois satélites usam a configuração de 4 MB definida em seus projetos.

## Compilação

Instale Python e PlatformIO. Na raiz do repositório, compile um alvo por vez:

```powershell
pio run -d firmware\mestre-s3 -e mestre-s3-n16r8
pio run -d firmware\equipe-s3 -e esp32-s3-devkitm-1
pio run -d firmware\visitantes-s3 -e esp32-s3-devkitm-1
```

Para a primeira gravação por cabo, acrescente `-t upload` ao comando do alvo. As
compilações ficam dentro de `.pio` e não devem ser adicionadas ao Git.

## Configuração

Antes da montagem ou gravação:

1. confira a [pinagem atual](hardware/pinagem/README.md);
2. copie `firmware/mestre-s3/include/nucleo/segredos_ota.exemplo.h` para
   `segredos_ota.h` e preencha apenas a cópia local;
3. revise os parâmetros da placa em
   `firmware/mestre-s3/include/nucleo/configuracao_placa.h`;
4. configure o Google somente se o registro remoto for utilizado;
5. mantenha GND comum entre o mestre e os satélites.

`segredos_ota.h`, binários, caches e configurações pessoais são ignorados pelo
Git. Os arquivos `*.exemplo.*` e `sdkconfig.defaults` são as referências que
devem permanecer versionadas.

## Comunicação e painéis

O quadro de telemetria v4 possui 105 bytes e está definido uma única vez em
`firmware/compartilhado/protocolos/protocolo_telemetria.h`. Os satélites validam
versão, tipo, tamanho e CRC-16 antes de publicar a amostra. Comandos e OTA usam
quadros próprios, confirmação, limite de tempo e repetição controlada.

Ao conectar à rede `UFSM-Carro-Equipe`, abra `http://192.168.4.1/`. As rotas
`/painel`, `/diagnostico` e `/mestre` entregam a mesma interface com três abas;
`/api/telemetria`, `/api/diagnostico` e `/api/mestre` fornecem os dados. O AP da
equipe não oferece DNS local: use o IP e HTTP explícito. O DHCP tenta anunciar o
portal pela opção 114, mas a abertura automática depende do cliente.

O módulo de visitantes cria a rede `UFSM-Carro-Visitantes` e serve a página
pública em `http://192.168.4.1/`. Sua API é apenas `/api/telemetria`. O DNS
cativo responde consultas IPv4 com o IP local quando disponível. Cada endereço
`192.168.4.1` pertence ao AP ao qual o dispositivo está conectado naquele momento.

## Documentação

- [Mapa de todos os guias](documentacao/README.md)
- [Arquitetura geral](documentacao/arquitetura/README.md)
- [Guia técnico do mestre](documentacao/firmware-mestre/README.md)
- [Guia técnico dos satélites e APIs](documentacao/satelites/README.md)
- [Arquitetura interna do mestre](firmware/mestre-s3/ARQUITETURA.md)
- [Atualização OTA](documentacao/ota/README.md)
- [Telemetria no Google Drive e Sheets](documentacao/telemetria-google/README.md)
- [Protocolos compartilhados](firmware/compartilhado/protocolos/README.md)
- [Pinagem](hardware/pinagem/README.md)

## Releases OTA

`python ferramentas\preparar_release_ota.py` detecta quais firmwares mudaram,
incrementa suas versões, compila as imagens e cria o manifesto. O procedimento
completo, a ordem segura de atualização e a publicação pelo GitHub Actions estão
no [guia de OTA](documentacao/ota/README.md).

Somente `versoes/estado_versoes.json` e os manifestos de exemplo são mantidos no
Git. Binários e arquivos preparados em `versoes/saida` pertencem ao GitHub
Release e são ignorados no repositório.
