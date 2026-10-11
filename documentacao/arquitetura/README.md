# Arquitetura do sistema

O sistema possui um mestre responsável pelos sensores e dois satélites dedicados
à apresentação. Essa separação mantém a aquisição independente da quantidade de
clientes Wi-Fi e permite atualizar ou reiniciar uma interface sem interromper as
demais funções do veículo.

```text
                    ┌─ UART da equipe ─────► painel da equipe
sensores ─► mestre ─┤
                    ├─ UART de visitantes ─► painel de visitantes
                    ├─ SPI ────────────────► cartão SD
                    └─ Wi-Fi temporário ───► OTA e Google
```

## Responsabilidades

Cada firmware é um projeto PlatformIO independente. Os contratos binários ficam
em `firmware/compartilhado`, evitando cópias divergentes entre os três projetos.

No mestre:

- `nucleo` reúne configurações de placa e políticas estáticas;
- `gerenciadores` encapsula drivers e periféricos do ESP-IDF;
- `dispositivos` representa componentes físicos, como MPU6050, encoders e ADC;
- `servicos` implementa regras de aplicação, diagnóstico, telemetria e OTA;
- `main.cpp` inicializa os módulos e coordena o fluxo periódico.

Nos satélites:

- `receptor_uart` valida e armazena o último retrato de telemetria;
- `gerenciador_uart` controla o transporte e as respostas ao mestre;
- `rede_wifi` mantém o ponto de acesso local;
- `servidor_web` apresenta telemetria e diagnóstico;
- no módulo da equipe, `painel_mestre_web` concentra a interface de comandos.

## Fluxos de dados

A aquisição produz um modelo em unidades de engenharia. O serviço de telemetria
o converte para o protocolo compacto e enfileira uma cópia para cada UART. Os
satélites só substituem o retrato publicado depois de validar cabeçalho, tamanho,
versão e CRC. As confirmações permitem medir atraso, perdas e disponibilidade.

O mesmo retrato é amostrado a 1 Hz pelo serviço de registro. Uma tarefa é a única
proprietária dos CSVs no cartão SD; por isso, aquisição e transmissão não esperam
gravações. O envio ao Google ocorre somente por solicitação e não faz parte do
caminho de tempo real.

Na atualização completa, o mestre transfere e confirma primeiro os dois
satélites. Sua própria atualização só começa depois dessa etapa, preservando o
controlador que coordena a operação até o fim.

## Falhas e recuperação

Gerenciadores retornam o `esp_err_t` original. Serviços decidem se devem repetir,
degradar uma função ou publicar uma falha; a aplicação decide se a inicialização
pode continuar. Diagnóstico e supervisão mantêm contadores e idade da última
resposta, enquanto OLED, terminal e painel web apresentam o estado sem acessar
diretamente os drivers.

Falhas de um recurso opcional, como tela, cartão SD ou Wi-Fi externo, não devem
interromper a telemetria UART. Falhas em tarefas e enlaces possuem recuperação
isolada. Operações remotas carregam um identificador de solicitação, e respostas
recentes são memorizadas para que uma repetição não execute novamente uma ação
com efeito colateral.

As regras detalhadas de dependência do mestre estão em
[`firmware/mestre-s3/ARQUITETURA.md`](../../firmware/mestre-s3/ARQUITETURA.md).
