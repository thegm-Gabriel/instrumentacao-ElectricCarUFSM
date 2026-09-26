# Firmware do ESP32-S3 mestre

Alvo de hardware: **ESP32-S3-WROOM-1-N16R8**, com 16 MB de flash Quad SPI e
8 MB de PSRAM Octal SPI.

Adquire tensão e MPU6050, monta o pacote de telemetria e o envia aos módulos da
equipe e de visitantes por UART. Também verifica atualizações no GitHub e só executa
o OTA após autorização pelo terminal. Consulte `ARQUITETURA.md` para a divisão
interna e `../../documentacao/ota/README.md` para preparar os releases.
