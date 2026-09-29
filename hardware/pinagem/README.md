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
 pinagem atual usa GPIO 4, 8 a 18, portanto não há conflito com a PSRAM.

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
