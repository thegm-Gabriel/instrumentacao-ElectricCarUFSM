#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "driver/uart.h"
#include "configuracao_enlace_uart.h"

namespace configuracao {

constexpr uint32_t TAMANHO_FLASH_ESPERADO_BYTES = 16U * 1024U * 1024U;
constexpr std::size_t TAMANHO_PSRAM_ESPERADO_BYTES = 8U * 1024U * 1024U;

// Aquisição atualmente implementada.
constexpr gpio_num_t PINO_LEITURA_TENSAO = GPIO_NUM_4;
constexpr i2c_port_t PORTA_I2C = I2C_NUM_0;
constexpr gpio_num_t PINO_I2C_SDA = GPIO_NUM_8;
constexpr gpio_num_t PINO_I2C_SCL = GPIO_NUM_9;
constexpr uint32_t FREQUENCIA_I2C_HZ = 400000;
constexpr uint8_t ENDERECO_MPU6050 = 0x68;
constexpr uint32_t TEMPO_LIMITE_I2C_MS = 100;

// Tela OLED SSD1306 I2C de 0,96 polegada, resolução 128x64.
// Ela compartilha SDA/SCL com o MPU6050; os endereços não conflitam.
constexpr uint8_t ENDERECO_TELA_SSD1306_PRINCIPAL = 0x3C;
constexpr uint8_t ENDERECO_TELA_SSD1306_ALTERNATIVO = 0x3D;
constexpr uint16_t LARGURA_TELA_SSD1306 = 128;
constexpr uint16_t ALTURA_TELA_SSD1306 = 64;
constexpr uint32_t INTERVALO_PAINEL_LOCAL_MS = 500;

// Sinalização visual e sonora. Os LEDs são externos e usam um resistor em série.
constexpr gpio_num_t PINO_LED_VERDE = GPIO_NUM_10;
constexpr gpio_num_t PINO_LED_AMARELO = GPIO_NUM_11;
constexpr gpio_num_t PINO_LED_VERMELHO = GPIO_NUM_12;
constexpr gpio_num_t PINO_LED_AZUL = GPIO_NUM_13;
constexpr bool LEDS_ATIVOS_EM_NIVEL_ALTO = true;

// O buzzer deve ser passivo para permitir tons e melodias por PWM.
constexpr gpio_num_t PINO_BUZZER_PASSIVO = GPIO_NUM_14;
constexpr ledc_mode_t MODO_PWM_BUZZER = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t TEMPORIZADOR_PWM_BUZZER = LEDC_TIMER_0;
constexpr ledc_channel_t CANAL_PWM_BUZZER = LEDC_CHANNEL_0;
constexpr ledc_timer_bit_t RESOLUCAO_PWM_BUZZER = LEDC_TIMER_10_BIT;
constexpr uint32_t FREQUENCIA_INICIAL_BUZZER_HZ = 2000;
constexpr uint8_t INTENSIDADE_BUZZER_PERCENTUAL = 65;

// Comunicação com o módulo da equipe.
constexpr uart_port_t UART_EQUIPE = UART_NUM_1;
constexpr gpio_num_t PINO_UART_EQUIPE_TX = GPIO_NUM_17;
constexpr gpio_num_t PINO_UART_EQUIPE_RX = GPIO_NUM_18;

// Comunicação com o módulo de visitantes.
constexpr uart_port_t UART_VISITANTES = UART_NUM_2;
constexpr gpio_num_t PINO_UART_VISITANTES_TX = GPIO_NUM_15;
constexpr gpio_num_t PINO_UART_VISITANTES_RX = GPIO_NUM_16;

constexpr int VELOCIDADE_UART = ENLACE_UART_BAUD_RATE;
constexpr int TAMANHO_BUFFER_UART_RX = 8192;
constexpr int TAMANHO_BUFFER_UART_TX = 8192;
constexpr std::size_t TAMANHO_MAXIMO_MENSAGEM_UART = 2176;
constexpr std::size_t CAPACIDADE_FILA_UART_TX = 4;
constexpr int INTERVALO_TELEMETRIA_MS = 500;
constexpr uint32_t INTERVALO_TELEMETRIA_OTA_PADRAO_MS = 2000;
constexpr uint32_t INTERVALO_TELEMETRIA_OTA_MINIMO_MS = 500;
constexpr uint32_t INTERVALO_TELEMETRIA_OTA_MAXIMO_MS = 10000;
constexpr bool USAR_TELEMETRIA_SIMULADA = true;
constexpr uint16_t DINAMICA_SIMULACAO_PERCENTUAL = 100;

}  // namespace configuracao
