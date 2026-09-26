# Arquitetura do ESP32-S3 mestre

O código ativo está dividido por responsabilidade:

- `nucleo`: pinagem, velocidades, tamanhos de buffer e recursos habilitados.
- `gerenciadores`: acesso genérico aos periféricos do ESP-IDF. Não conhecem o formato da telemetria.
- `dispositivos`: regras específicas de cada componente físico, como MPU6050 e entrada ADC.
- `servicos`: funcionalidades da aplicação, como montagem e envio do pacote de telemetria.
- `main.cpp`: inicialização e coordenação periódica. Não acessa periféricos diretamente.

## Estado atual

UART, I2C e OTA possuem implementação funcional. I2S e cartão SD possuem contratos,
validação de argumentos, estados e códigos de erro, mas permanecem desabilitados em
`nucleo/configuracao_placa.h`. Eles devem ser completados depois que forem definidos:

- I2S: direção, formato, DMA e pinos de clock/dados.
- SD: SPI ou SDMMC, pinagem, ponto de montagem e política de gravação.

O OTA usa Wi-Fi em modo estação, HTTPS com o pacote de certificados do ESP-IDF,
manifesto no GitHub Release, SHA-256, duas partições de aplicação e rollback. A
orquestração e a autorização ficam em `servicos/servico_ota.cpp`; acesso a Wi-Fi e
gravação da imagem ficam nos respectivos gerenciadores.

Os gerenciadores retornam `esp_err_t`. O código chamador deve sempre verificar o
resultado antes de usar os dados ou avançar para a próxima etapa.
