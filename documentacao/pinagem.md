# Pinagem atual

[Índice da documentação](README.md)

Todas as UARTs usam nível lógico de 3,3 V, formato 8N1 e 1.000.000 baud. Os três
ESP32-S3 precisam compartilhar o GND. Os firmwares devem usar a mesma taxa,
definida uma única vez em
`firmware/compartilhado/protocolos/configuracao_enlace_uart.h`.

O mestre usa o módulo ESP32-S3-WROOM-1-N16R8. Como a PSRAM é Octal, reserve os
GPIOs 33 a 37 para a interface interna de memória e não os use em periféricos. A
pinagem também usa GPIO 38 a 42, 47 e 48, sem ocupar os pinos da PSRAM.

## Entrada ADC do mestre

| Função | GPIO do mestre | Implementação atual |
|---|---:|---|
| Leitura analógica de tensão | GPIO 4 | ADC oneshot, atenuação de 12 dB; publica a contagem bruta de 0 a 4095 |

`leitor_tensao.cpp` não aplica calibração nem converte essa contagem em volts.
Qualquer tensão externa precisa de condicionamento elétrico apropriado para a
entrada de 3,3 V do ESP32-S3; os campos de tensão do pacote do veículo não devem
ser interpretados como medição do conjunto de baterias a partir desse ADC.

## LEDs e buzzer do mestre

| Função | GPIO do mestre | Ligação recomendada |
|---|---:|---|
| LED verde | GPIO 10 | GPIO → resistor de 220 a 330 Ω → ânodo; cátodo → GND |
| LED amarelo | GPIO 11 | GPIO → resistor de 220 a 330 Ω → ânodo; cátodo → GND |
| LED vermelho | GPIO 12 | GPIO → resistor de 220 a 330 Ω → ânodo; cátodo → GND |
| LED azul | GPIO 13 | GPIO → resistor de 220 a 330 Ω → ânodo; cátodo → GND |
| Buzzer passivo | GPIO 14 | Saída PWM; use o circuito de acionamento abaixo |

O buzzer usado pelo firmware é o **passivo**, pois o tom é produzido por PWM e
pode variar de frequência. Para não exigir corrente do GPIO, use um transistor
NPN: GPIO 14 → resistor de 1 kΩ → base; emissor → GND; coletor → terminal negativo
do buzzer; terminal positivo → alimentação indicada no componente. O GND da fonte
do buzzer e do ESP deve ser comum. Para buzzer eletromagnético, coloque também um
diodo de proteção em paralelo com o buzzer. Um piezo passivo de baixa corrente pode
ser ligado diretamente somente se a corrente declarada pelo fabricante estiver
dentro do limite do GPIO.

Os pinos podem ser alterados em
`firmware/mestre-s3/include/nucleo/configuracao_placa.h` sem mudar o serviço.

## Encoders incrementais do mestre

Cada encoder E38S6G5-600B-G24N é alimentado por uma fonte externa compatível (12 V
é recomendado) e suas saídas NPN de coletor aberto recebem pull-ups externos de
4,7 kΩ para **3,3 V**. O GND da fonte e o GND do ESP32-S3 devem ser comuns.

| Encoder/sinal | ESP32-S3 mestre | Observação |
|---|---:|---|
| Esquerdo A | GPIO 5 | Pull-up de 4,7 kΩ para 3,3 V |
| Esquerdo B | GPIO 6 | Pull-up de 4,7 kΩ para 3,3 V |
| Direito A | GPIO 7 | Pull-up de 4,7 kΩ para 3,3 V |
| Direito B | GPIO 21 | Pull-up de 4,7 kΩ para 3,3 V |
| GND/preto de ambos | GND | Comum à fonte dos encoders |
| Alimentação/vermelho | Fonte externa | Não ligar ao GPIO |
| Blindagem | GND | Preferencialmente apenas no lado do controlador |

Nunca faça o pull-up de A ou B para 12/24 V. O periférico PCNT conta todas as
bordas dos dois canais de cada encoder (2400 contagens por giro para 600 P/R) sem
consultar os pinos continuamente pela CPU. A configuração atual soma pulsos,
mas não usa a defasagem entre A e B para identificar o sentido de rotação:
deslocamentos para frente e para trás aumentam o percurso registrado. O
odômetro usa a média das distâncias esquerda e direita. Diâmetros, relações
de transmissão e intervalo de persistência ficam em
`firmware/mestre-s3/include/nucleo/configuracao_placa.h`.

## Tela SSD1306 I²C do mestre

A tela OLED de 0,96 polegada deve ser a versão I²C de quatro pinos e resolução
128×64. Alguns módulos identificam o clock como `SCK`; nessa tela ele corresponde
ao sinal `SCL` do barramento I²C.

| Pino da SSD1306 | Ligação no mestre |
|---|---|
| VCC | 3,3 V |
| GND | GND |
| SDA | GPIO 8, compartilhado com o MPU6050 |
| SCK/SCL | GPIO 9, compartilhado com o MPU6050 |

A SSD1306 e o MPU6050 ficam ligados em paralelo em SDA e SCL. Os endereços usados
são distintos: a tela é detectada em `0x3C` ou `0x3D`, enquanto o MPU6050 usa
`0x68`. Os módulos precisam compartilhar a mesma referência de GND. Se a placa da
tela possuir pull-ups para 5 V, alimente-a em 3,3 V para não aplicar 5 V aos GPIOs
do ESP32-S3.

## Mestre e módulo da equipe

| Sinal | ESP32-S3 mestre | ESP32-S3 equipe |
|---|---:|---:|
| Mestre TX → equipe RX | GPIO 17 | GPIO 5 |
| Mestre RX ← equipe TX | GPIO 18 | GPIO 6 |
| Referência elétrica | GND | GND |

## Mestre e módulo de visitantes

| Sinal | ESP32-S3 mestre | ESP32-S3 visitantes |
|---|---:|---:|
| Mestre TX → visitantes RX | GPIO 15 | GPIO 5 |
| Mestre RX ← visitantes TX | GPIO 16 | GPIO 6 |
| Referência elétrica | GND | GND |

Os dois fios de retorno são necessários no firmware atual: os satélites enviam
ACK de telemetria, respostas a comandos e confirmações de blocos OTA. Ligue TX de
cada satélite ao RX correspondente do mestre e mantenha GND comum.

## Cartão microSD do mestre

O registro histórico usa um módulo microSD em 3,3 V pelo barramento SPI2. Os
GPIOs 33 a 37 continuam reservados para a PSRAM Octal.

| Sinal do módulo | ESP32-S3 mestre |
|---|---:|
| CLK | GPIO 38 |
| MOSI / DI | GPIO 39 |
| MISO / DO | GPIO 40 |
| CS | GPIO 41 |
| VCC | 3,3 V |
| GND | GND |

Esses pinos são definidos em
[`configuracao_registro_telemetria.h`](../firmware/mestre-s3/include/nucleo/configuracao_registro_telemetria.h)
e precisam ser conferidos com a placa e o módulo usados antes de fabricar o
chicote. O comportamento dos arquivos e do envio está no
[guia do histórico no Google](google.md).

## Microfone INMP441 temporário para treinamento

A opção 9 do terminal grava amostras WAV para `/sdcard/audio_ia`. Esta ligação é
temporária para formar o conjunto de treinamento do reconhecedor de voz.

| Pino do INMP441 | ESP32-S3 mestre | Observação |
|---|---:|---|
| VDD | 3,3 V | Não alimentar com 5 V |
| GND | GND | Referência comum |
| SCK / BCLK | GPIO 42 | Clock I2S |
| WS / LRCL | GPIO 47 | Seleção de palavra I2S |
| SD | GPIO 48 | Dados do microfone para o mestre |
| L/R | GND | Seleciona o canal esquerdo configurado no firmware |

Os pinos ficam centralizados em `configuracao_placa.h`. O gravador produz áudio
mono PCM de 16 bits a 16 kHz, não sobrescreve amostras existentes e aceita de 1
a 60 segundos por arquivo.
