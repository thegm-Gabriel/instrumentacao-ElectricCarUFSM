# Pinagem atual

Todas as UARTs usam nível lógico de 3,3 V, formato 8N1 e 460800 baud. Os três
ESP32-S3 precisam compartilhar o GND.

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
