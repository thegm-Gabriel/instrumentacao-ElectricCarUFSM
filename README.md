# Instrumentação do carro elétrico

Repositório dos firmwares e documentos da instrumentação e telemetria do carro
elétrico de pequeno porte da UFSM-CS.

## Estrutura

```text
instrumentacao-carro/
├── firmware/
│   ├── mestre-s3/
│   ├── equipe-s3/
│   ├── visitantes-s3/
│   └── compartilhado/
│       ├── protocolos/
│       ├── telemetria/
│       └── comum/
├── hardware/
│   ├── kicad/
│   ├── folhas-de-dados/
│   └── pinagem/
├── documentacao/
│   ├── arquitetura/
│   └── ota/
└── versoes/
```

Os três diretórios em `firmware` são projetos PlatformIO independentes com ESP-IDF.
O mestre usa um ESP32-S3-WROOM-1-N16R8, com 16 MB de flash e 8 MB de PSRAM; os
módulos da equipe e de visitantes mantêm suas configurações próprias de placa e
memória. Abra o diretório do firmware desejado no VS Code ou use o PlatformIO a
partir dele.

O pacote UART v3 de 88 bytes, protegido por CRC-16, é definido uma única vez em
`firmware/compartilhado/protocolos/protocolo_telemetria.h`. Os três projetos incluem esse
arquivo por meio de seus respectivos `platformio.ini` e `src/CMakeLists.txt`.

Consulte `hardware/pinagem/README.md` antes das ligações e mantenha GND comum entre
as placas.

O mestre consulta um único manifesto no GitHub Release e pode atualizar a si mesmo
ou, pela UART, cada um dos dois satélites. Todas as instalações exigem autorização
no terminal, validam SHA-256 e usam rollback. A preparação inicial está documentada
em `documentacao/ota/README.md`.
