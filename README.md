# Instrumentação do carro elétrico

Repositório dos firmwares e documentos da instrumentação e telemetria do carro
elétrico de pequeno porte da UFSM-CS.

## Estrutura

```text
instrumentacao-carro/
├── firmware/
│   ├── mestre-s3/
│   ├── equipe-s3/
│   └── visitantes-s3/
├── compartilhado/
│   ├── protocolos/
│   ├── telemetria/
│   └── comum/
├── hardware/
│   ├── kicad/
│   ├── folhas-de-dados/
│   └── pinagem/
├── documentacao/
│   ├── arquitetura/
│   └── ota/
└── versoes/
```

Os três diretórios em `firmware` são projetos PlatformIO independentes para ESP32-S3
com ESP-IDF e flash de 4 MB. Abra o diretório do firmware desejado no VS Code ou use
o PlatformIO a partir dele.

O pacote UART de 23 bytes é definido uma única vez em
`compartilhado/protocolos/protocolo_telemetria.h`. Os três projetos incluem esse
arquivo por meio de seus respectivos `platformio.ini` e `src/CMakeLists.txt`.

Consulte `hardware/pinagem/README.md` antes das ligações e mantenha GND comum entre
as placas.
