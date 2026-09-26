# Arquitetura do ESP32-S3 mestre

O código ativo está dividido por responsabilidade:

- `nucleo`: pinagem, velocidades, tamanhos de buffer e recursos habilitados.
- `gerenciadores`: acesso genérico aos periféricos do ESP-IDF. Não conhecem o formato da telemetria.
- `dispositivos`: regras específicas de cada componente físico, como MPU6050 e entrada ADC.
- `servicos`: funcionalidades da aplicação, como montagem e envio do pacote de telemetria.
- `main.cpp`: inicialização e coordenação periódica. Não acessa periféricos diretamente.

## Estado atual

UART e I2C possuem implementação funcional. I2S, OTA e cartão SD possuem contratos,
validação de argumentos, estados e códigos de erro, mas permanecem desabilitados em
`nucleo/configuracao_placa.h`. Eles devem ser completados depois que forem definidos:

- I2S: direção, formato, DMA e pinos de clock/dados.
- OTA: conexão de rede, servidor HTTPS, certificado e tabela de partições.
- SD: SPI ou SDMMC, pinagem, ponto de montagem e política de gravação.

Os gerenciadores retornam `esp_err_t`. O código chamador deve sempre verificar o
resultado antes de usar os dados ou avançar para a próxima etapa.
