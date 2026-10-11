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
ESP32-S3 mestre ── UART ──► ESP32-S3 da equipe ── Wi-Fi ──► painel.equipe
        │                              └───────────────► painel.mestre
        ├────────── UART ──► ESP32-S3 de visitantes ─► portal público
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

Ao conectar à rede `UFSM-Carro-Equipe`, use HTTP, não HTTPS:

- `http://painel.equipe` — telemetria de condução e energia;
- `http://painel.diagnostico` — estado técnico do satélite e da comunicação;
- `http://painel.mestre` — consultas e operações confirmadas no mestre.

Esses nomes são respondidos pelo DNS local do próprio satélite e só funcionam
enquanto o dispositivo estiver conectado ao ponto de acesso da equipe. Caso o
navegador force HTTPS ou DNS seguro, desative essa opção para a rede local e
abra novamente o endereço com `http://` explícito.

## Documentação

- [Arquitetura geral](documentacao/arquitetura/README.md)
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
