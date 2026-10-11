# Pinagem atual

Todas as UARTs usam nível lógico de 3,3 V, formato 8N1 e 1.000.000 baud. Os três
ESP32-S3 precisam compartilhar o GND. Os firmwares devem usar a mesma taxa,
definida uma única vez em
`firmware/compartilhado/protocolos/configuracao_enlace_uart.h`.

Ao migrar equipamentos já gravados em 921600 baud por OTA, atualize primeiro os
dois satélites usando o mestre antigo e atualize o mestre por último. Durante
essa transição, um satélite já atualizado ficará temporariamente sem enlace com
o mestre antigo.

O mestre usa o módulo ESP32-S3-WROOM-1-N16R8. Como a PSRAM é Octal, reserve os
GPIOs 33 a 37 para a interface interna de memória e não os use em periféricos. A
 pinagem atual usa GPIO 4 a 18 e 21, portanto não há conflito com a PSRAM.

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
polling da CPU e soma a distância nos dois sentidos. O odômetro usa a média das
distâncias esquerda e direita. Diâmetros, relações de transmissão e intervalo de persistência ficam em
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

Atualmente os módulos da equipe e de visitantes apenas recebem telemetria, mas os
fios de retorno já podem ficar previstos para uso futuro.

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

Esses pinos são uma definição de firmware e precisam ser conferidos com a placa
e o módulo usados antes de fabricar o chicote.
