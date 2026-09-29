# Arquitetura do sistema

O ESP32-S3 mestre lê os sensores e transmite o mesmo pacote binário para duas UARTs.
Os módulos da equipe e de visitantes recebem os quadros, validam o CRC-16 e publicam
os valores em seus próprios pontos de acesso Wi-Fi e painéis web.

Cada firmware é um projeto PlatformIO independente. O contrato binário usado entre
eles fica em `firmware/compartilhado/protocolos`, evitando três definições divergentes.

No firmware mestre:

- `gerenciadores` concentra acesso genérico aos periféricos;
- `dispositivos` implementa componentes físicos, como MPU6050 e ADC;
- `servicos` implementa funcionalidades, como a telemetria;
- `nucleo` reúne a configuração da placa;
- `main.cpp` inicializa e coordena os módulos.
