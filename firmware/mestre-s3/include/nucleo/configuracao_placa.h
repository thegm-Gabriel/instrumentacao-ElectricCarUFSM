#pragma once

#include <cstdint>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/uart.h"

namespace configuracao {

// Aquisição atualmente implementada.
constexpr gpio_num_t PINO_LEITURA_TENSAO = GPIO_NUM_4;
constexpr i2c_port_t PORTA_I2C = I2C_NUM_0;
constexpr gpio_num_t PINO_I2C_SDA = GPIO_NUM_8;
constexpr gpio_num_t PINO_I2C_SCL = GPIO_NUM_9;
constexpr uint32_t FREQUENCIA_I2C_HZ = 400000;
constexpr uint8_t ENDERECO_MPU6050 = 0x68;
constexpr uint32_t TEMPO_LIMITE_I2C_MS = 100;

// Comunicação com o módulo da equipe.
constexpr uart_port_t UART_EQUIPE = UART_NUM_1;
constexpr gpio_num_t PINO_UART_EQUIPE_TX = GPIO_NUM_17;
constexpr gpio_num_t PINO_UART_EQUIPE_RX = GPIO_NUM_18;

// Comunicação com o módulo de visitantes.
constexpr uart_port_t UART_VISITANTES = UART_NUM_2;
constexpr gpio_num_t PINO_UART_VISITANTES_TX = GPIO_NUM_15;
constexpr gpio_num_t PINO_UART_VISITANTES_RX = GPIO_NUM_16;

constexpr int VELOCIDADE_UART = 460800;
constexpr int TAMANHO_BUFFER_UART_RX = 1024;
constexpr int TAMANHO_BUFFER_UART_TX = 1024;
constexpr uint32_t TEMPO_LIMITE_UART_MS = 20;
constexpr int INTERVALO_TELEMETRIA_MS = 500;

// I2S, SD e OTA permanecem desabilitados até a definição de hardware e política.
constexpr bool HABILITAR_I2S = false;
constexpr bool HABILITAR_CARTAO_SD = false;
constexpr bool HABILITAR_OTA = false;

}  // namespace configuracao
