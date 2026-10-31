# Mapa da documentação

Esta pasta reúne os guias transversais. Os READMEs junto ao código apresentam
cada módulo; os cabeçalhos de configuração e protocolo continuam sendo a fonte
de verdade para constantes, campos e pinagem.

| Tema | Documento | Conteúdo |
|---|---|---|
| Visão geral, compilação e acesso aos painéis | [README da raiz](../README.md) | Estrutura do repositório e primeiros passos. |
| Arquitetura do sistema | [Arquitetura geral](arquitetura/README.md) | Responsabilidades e fluxos entre os três ESP32-S3. |
| Firmware mestre | [Guia técnico do mestre](firmware-mestre/README.md) | Aquisição, serviços, tarefas, estados, pinout, SD, Wi-Fi, OTA e terminal. |
| Satélites e HTTP | [Guia técnico dos satélites](satelites/README.md) | Enlace bidirecional, painéis, endpoints e limitações. |
| Ligações físicas | [Pinagem](../hardware/pinagem/README.md) | GPIOs e recomendações elétricas. |
| Protocolos binários | [Protocolos compartilhados](../firmware/compartilhado/protocolos/README.md) | Telemetria, ACK, comandos e OTA UART. |
| Atualização | [OTA](ota/README.md) | Manifesto, release, autorização, transferência e rollback. |
| Registro remoto | [Telemetria no Google](telemetria-google/README.md) | CSV, envio e integração Drive/Sheets. |
| Regras internas do mestre | [Arquitetura do mestre](../firmware/mestre-s3/ARQUITETURA.md) | Dependências e recuperação. |

## READMEs junto aos componentes

- [Mestre](../firmware/mestre-s3/README.md),
  [equipe](../firmware/equipe-s3/README.md) e
  [visitantes](../firmware/visitantes-s3/README.md): uso de cada firmware.
- [Telemetria compartilhada](../firmware/compartilhado/telemetria/README.md),
  [código comum](../firmware/compartilhado/comum/README.md) e
  [teste de protocolo](../firmware/mestre-s3/test/test_protocolo_telemetria/README.md).
- [KiCad](../hardware/kicad/README.md) e
  [folhas de dados](../hardware/folhas-de-dados/README.md).

Ao alterar uma função, atualize primeiro o código e o documento junto ao
componente. Mudanças de GPIO, constantes de rede e campos binários também
exigem revisão da pinagem, dos protocolos e dos guias técnicos correspondentes.
