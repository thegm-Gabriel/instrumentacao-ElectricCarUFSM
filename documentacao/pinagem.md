# Pinagem atual

[Índice da documentação](README.md)

Os enlaces UART entre o Mestre ESP32-S3 e os satélites ESP32-C3 usam nível lógico de 3,3 V, formato 8N1 e
1.000.000 baud. Ligue o GND dos três módulos em comum. A taxa dos enlaces
vem da configuração compartilhada. O console de diagnóstico do Mestre usa
115200 baud. Consulte [`configuracao_enlace_uart.h`](../firmware/compartilhado/protocolos/configuracao_enlace_uart.h).

O mestre usa o módulo ESP32-S3-WROOM-1-N16R8. Como a PSRAM é Octal, reserve os
GPIOs 33 a 37 para a interface interna de memória. A pinagem do firmware,
incluindo os GPIOs 2/42/1/41 usados pelo microSD, fica em
[`placa.h`](../firmware/mestre-s3/include/nucleo/configuracao/placa.h).

## Entrada ADC do mestre

| Função | GPIO do mestre | Implementação atual |
|---|---:|---|
| Pedal Hall | GPIO 5 | ADC1 calibrado. A tarefa de controle lê milivolts e o diagnóstico publica contagens brutas do mesmo canal. |

O INA228 mede a tensão do banco. O Mestre usa o ADC apenas para o pedal Hall,
com curso nominal de 0,5 a 2,5 V, dentro da faixa de entrada do ESP32-S3.
Consulte [`adc.cpp`](../firmware/mestre-s3/src/dispositivos/entradas/adc.cpp).

## Interface de controle da tração

| Sinal | Configuração do Mestre | Situação |
|---|---|---|
| MCP4725 SDA/SCL | GPIO 7 / GPIO 6, I²C em `0x60` | Parte em 0 V e atua quando a tração é habilitada. |
| Chave física A/B | GPIO 16 / GPIO 15 | A baixo seleciona L, B baixo seleciona H, ambos altos selecionam M. Ambos baixos bloqueiam a saída. |
| Seletor de ré | GPIO 4 | Entrada ativa em nível baixo. |
| Modos A/B, ré, relé de habilitação | GPIO 14 / 13 / 47 / 21 | Saídas ativas em nível alto. O relé usa o contato normalmente aberto. |

O lado do ESP usa 3,3 V. Com o DAC alimentado em 5 V, a interface SDA/SCL
requer conversão de nível bidirecional e pull-ups na tensão de cada lado.
O projeto prevê corte do driver por contato normalmente aberto. Confirme o
estado do circuito durante partida, reset e falta de alimentação antes de
conectar o controlador de tração. Consulte [controle remoto](controle-remoto.md)
e [`placa.h`](../firmware/mestre-s3/include/nucleo/configuracao/placa.h).

A pinagem do firmware segue os sinais do esquema do Mestre. Consulte a [PCB em KiCad](../hardware/kicad/KiCad_pcb_telemetria/KiCad_pcb_telemetria.kicad_pcb).

## Monitor elétrico INA228 do mestre

O INA228 compartilha o I²C0 a 400 kHz com MPU6050 e OLED: SDA no GPIO 7,
SCL no GPIO 6, alimentação lógica compatível com 3,3 V e GND comum.
O firmware usa o endereço `0x40`, obtido com **A0 e A1 em GND**. O shunt
configurado é de **1 mΩ**, com corrente máxima esperada de **75 A**; confirme
o valor real e a conexão Kelvin na placa montada. Os terminais `IN+` e `IN−`
devem ficar nos lados correspondentes do shunt; o sentido configurado considera
corrente positiva como consumo. `VBUS` deve ser ligado ao ponto cuja tensão se
quer publicar. O INA228 aceita até 85 V no barramento, mas o protocolo UART
atual representa no máximo 65,535 V; valores maiores são recusados como
leitura publicável. O pino `ALERT` não é usado: os sinalizadores de diagnóstico
são lidos pelo I²C, sem atuação física de proteção.

## LEDs e buzzer do mestre

| Função | GPIO do mestre | Ligação recomendada |
|---|---:|---|
| LED verde | GPIO 11 | GPIO → resistor de 220 a 330 Ω → ânodo; cátodo → GND |
| LED vermelho | GPIO 10 | GPIO → resistor de 220 a 330 Ω → ânodo; cátodo → GND |
| LED amarelo | GPIO 12 | Saída `LED_BLUE_CMD` no esquema. A cor é a do LED instalado. |
| LED RGB integrado do ESP32-S3 | GPIO 48 | LED endereçável comandado pelo periférico RMT. |
| Buzzer passivo | GPIO 38 | Saída PWM; use o circuito de acionamento abaixo |

O buzzer usado pelo firmware é o **passivo**, pois o tom é produzido por PWM e
pode variar de frequência. Para não exigir corrente do GPIO, use um transistor
NPN: GPIO 38 → resistor de 1 kΩ → base; emissor → GND; coletor → terminal negativo
do buzzer; terminal positivo → alimentação indicada no componente. O GND da fonte
do buzzer e do ESP deve ser comum. Para buzzer eletromagnético, coloque também um
diodo de proteção em paralelo com o buzzer. Um piezo passivo de baixa corrente pode
ser ligado diretamente somente se a corrente declarada pelo fabricante estiver
dentro do limite do GPIO.

Os pinos podem ser alterados em
`firmware/mestre-s3/include/nucleo/configuracao/placa.h` sem mudar o serviço.

## Encoders incrementais do mestre

Cada encoder E38S6G5-600B-G24N é alimentado por uma fonte externa compatível (12 V
é recomendado) e suas saídas NPN de coletor aberto recebem pull-ups externos de
4,7 kΩ para **3,3 V**. O GND da fonte e o GND do ESP32-S3 devem ser comuns.

| Encoder/sinal | ESP32-S3 mestre | Observação |
|---|---:|---|
| Esquerdo A | GPIO 17 | Pull-up de 4,7 kΩ para 3,3 V |
| Esquerdo B | GPIO 18 | Pull-up de 4,7 kΩ para 3,3 V |
| Direito A | GPIO 8 | Pull-up de 4,7 kΩ para 3,3 V |
| Direito B | GPIO 9 | Pull-up de 4,7 kΩ para 3,3 V |
| GND/preto de ambos | GND | Comum à fonte dos encoders |
| Alimentação/vermelho | Fonte externa | Não ligar ao GPIO |
| Blindagem | GND | Preferencialmente apenas no lado do controlador |

Nunca faça o pull-up de A ou B para 12/24 V. O periférico PCNT conta todas as
bordas dos dois canais de cada encoder (2400 contagens por giro para 600 P/R) sem
consultar os pinos continuamente pela CPU. A configuração atual soma pulsos,
mas não usa a defasagem entre A e B para identificar o sentido de rotação:
deslocamentos para frente e para trás aumentam o percurso registrado. O
odômetro usa a média das distâncias esquerda e direita. Na lógica de tração,
o limitador usa a velocidade da roda mais rápida. O firmware mede o
movimento a partir das bordas. Ausência de pulsos nas duas entradas pode
representar repouso ou interrupção dos sinais. Confira alimentação, canais
A/B e detecção dessa falha em bancada. Diâmetros, relações de transmissão e
intervalo de persistência ficam em
[`veiculo.h`](../firmware/mestre-s3/include/nucleo/configuracao/veiculo.h).

## Tela SSD1306 I²C do mestre

A tela OLED de 0,96 polegada deve ser a versão I²C de quatro pinos e resolução
128×64. Alguns módulos identificam o clock como `SCK`; nessa tela ele corresponde
ao sinal `SCL` do barramento I²C.

| Pino da SSD1306 | Ligação no mestre |
|---|---|
| VCC | 3,3 V |
| GND | GND |
| SDA | GPIO 7, compartilhado com o MPU6050 |
| SCK/SCL | GPIO 6, compartilhado com o MPU6050 |

A SSD1306 e o MPU6050 ficam ligados em paralelo em SDA e SCL. Os endereços usados
são distintos: a tela é detectada em `0x3C` ou `0x3D`, enquanto o MPU6050 usa
`0x68`. Os módulos precisam compartilhar a mesma referência de GND. Se a placa da
tela possuir pull-ups para 5 V, alimente-a em 3,3 V para não aplicar 5 V aos GPIOs
do ESP32-S3.

## Mestre e módulo da equipe

| Sinal | ESP32-S3 mestre | ESP32-C3 Equipe |
|---|---:|---:|
| Mestre TX → equipe RX | GPIO 39 | GPIO 5 |
| Mestre RX ← equipe TX | GPIO 40 | GPIO 6 |
| Referência elétrica | GND | GND |

## Mestre e módulo de visitantes

| Sinal | ESP32-S3 mestre | ESP32-C3 Visitantes |
|---|---:|---:|
| Mestre TX → visitantes RX | GPIO 19 | GPIO 5 |
| Mestre RX ← visitantes TX | GPIO 20 | GPIO 6 |
| Referência elétrica | GND | GND |

Os dois fios de retorno são necessários no firmware atual: os satélites enviam
ACK de telemetria, respostas a comandos e confirmações de blocos OTA. Ligue TX de
cada satélite ao RX correspondente do mestre e mantenha GND comum.

## Cartão microSD do mestre

O registro histórico usa um módulo microSD em 3,3 V pelo barramento SPI2. Os
GPIOs 33 a 37 continuam reservados para a PSRAM Octal.

| Sinal do módulo | ESP32-S3 mestre |
|---|---:|
| CLK | GPIO 2 |
| MOSI / DI | GPIO 42 |
| MISO / DO | GPIO 1 |
| CS | GPIO 41 |
| VCC | 3,3 V |
| GND | GND |

Esses pinos são definidos em
[`placa.h`](../firmware/mestre-s3/include/nucleo/configuracao/placa.h)
e precisam ser conferidos com a placa e o módulo usados antes de fabricar o
chicote. A velocidade padrão é 10 MHz, com tentativa a 400 kHz quando a
montagem falha por comunicação. Se o log indicar falha em `f_mount` após
inicializar o cartão, verifique o volume FAT32. O comportamento dos arquivos e do envio está no
[guia do histórico no Google](google.md).
