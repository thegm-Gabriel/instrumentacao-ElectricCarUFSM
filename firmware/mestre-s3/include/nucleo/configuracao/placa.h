// Centraliza pinagem e limites físicos da placa mestre.
#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "driver/uart.h"

namespace configuracao {

// Memória da placa
// Capacidade esperada da flash e da PSRAM para o perfil N16R8.
constexpr uint32_t TAMANHO_FLASH_ESPERADO_BYTES    = 16U * 1024U * 1024U;
constexpr std::size_t TAMANHO_PSRAM_ESPERADO_BYTES = 8U * 1024U * 1024U;

// Entradas analógicas
// GPIOs do ADC usados para tensão e pedal.
constexpr gpio_num_t PINO_LEITURA_TENSAO = GPIO_NUM_4;
// Pedal Hall medido em 0,5–2,5 V. GPIO3 é ADC1_CH2 e pino de strapping:
// verificar o nível durante a partida antes de ligar o veículo.
constexpr gpio_num_t PINO_ACELERADOR_ADC = GPIO_NUM_3;

// Chave de marcha e saídas do veículo
// Dois fios da chave de marcha, condicionados para 3,3 V: L ou H em nível
// baixo; ambos altos indicam M. Não conectar os fios de 5 V ao ESP.
constexpr bool HABILITAR_LEITURA_CHAVE_MARCHA = false;
constexpr gpio_num_t PINO_CHAVE_MARCHA_L      = GPIO_NUM_NC;
constexpr gpio_num_t PINO_CHAVE_MARCHA_H      = GPIO_NUM_NC;
constexpr gpio_num_t PINO_RELE_HABILITADOR    = GPIO_NUM_NC;
constexpr gpio_num_t PINO_MARCHA_A            = GPIO_NUM_NC;
constexpr gpio_num_t PINO_MARCHA_B            = GPIO_NUM_NC;
constexpr gpio_num_t PINO_REVERSAO            = GPIO_NUM_NC;

// DAC MCP4725
// Alimentação usada na conversão para milivolts e endereço I2C.
constexpr uint16_t MCP4725_ALIMENTACAO_MV = 5000;
constexpr uint8_t ENDERECO_MCP4725        = 0x60;

// Barramento I2C
// Pinos, frequência, endereços e parâmetros elétricos dos sensores.
constexpr i2c_port_t PORTA_I2C       = I2C_NUM_0;
constexpr gpio_num_t PINO_I2C_SDA    = GPIO_NUM_8;
constexpr gpio_num_t PINO_I2C_SCL    = GPIO_NUM_9;
constexpr uint32_t FREQUENCIA_I2C_HZ = 400000;
constexpr uint8_t ENDERECO_MPU6050   = 0x68;
// INA228: A0/A1 em GND. Confirmar shunt e sentido na montagem final.
constexpr uint8_t ENDERECO_INA228            = 0x40;
constexpr float RESISTENCIA_SHUNT_INA228_OHM = 0.001f;
constexpr float CORRENTE_MAXIMA_INA228_A     = 75.0f;
constexpr float SINAL_CORRENTE_INA228        = 1.0f;

// Tempo limite de uma operação no barramento.
constexpr uint32_t TEMPO_LIMITE_I2C_MS = 100;

// Encoders incrementais E38S6G5-600B-G24N. As entradas A/B exigem pull-ups
// externos para 3,3 V; nunca aplique 12/24 V aos GPIOs do ESP32-S3.
constexpr gpio_num_t PINO_ENCODER_ESQUERDO_A = GPIO_NUM_5;
constexpr gpio_num_t PINO_ENCODER_ESQUERDO_B = GPIO_NUM_6;
constexpr gpio_num_t PINO_ENCODER_DIREITO_A  = GPIO_NUM_7;
constexpr gpio_num_t PINO_ENCODER_DIREITO_B  = GPIO_NUM_21;

// Tela OLED
// Tela OLED SSD1306 I2C de 0,96 polegada, resolução 128x64.
// Ela compartilha SDA/SCL com o MPU6050; os endereços não conflitam.
constexpr uint8_t ENDERECO_TELA_SSD1306_PRINCIPAL   = 0x3C;
constexpr uint8_t ENDERECO_TELA_SSD1306_ALTERNATIVO = 0x3D;
constexpr uint16_t LARGURA_TELA_SSD1306             = 128;
constexpr uint16_t ALTURA_TELA_SSD1306              = 64;

// Sinalização visual e sonora. Os LEDs são externos e usam um resistor em série.
constexpr gpio_num_t PINO_LED_VERDE      = GPIO_NUM_10;
constexpr gpio_num_t PINO_LED_AMARELO    = GPIO_NUM_11;
constexpr gpio_num_t PINO_LED_VERMELHO   = GPIO_NUM_12;
constexpr gpio_num_t PINO_LED_AZUL       = GPIO_NUM_13;
constexpr bool LEDS_ATIVOS_EM_NIVEL_ALTO = true;

// O buzzer deve ser passivo para permitir tons e melodias por PWM.
constexpr gpio_num_t PINO_BUZZER_PASSIVO        = GPIO_NUM_14;
constexpr ledc_mode_t MODO_PWM_BUZZER           = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t TEMPORIZADOR_PWM_BUZZER  = LEDC_TIMER_0;
constexpr ledc_channel_t CANAL_PWM_BUZZER       = LEDC_CHANNEL_0;
constexpr ledc_timer_bit_t RESOLUCAO_PWM_BUZZER = LEDC_TIMER_10_BIT;
constexpr uint32_t FREQUENCIA_INICIAL_BUZZER_HZ = 2000;
constexpr uint8_t INTENSIDADE_BUZZER_PERCENTUAL = 65;

// UART dos satélites
// Comunicação com o módulo da equipe.
constexpr uart_port_t UART_EQUIPE        = UART_NUM_1;
constexpr gpio_num_t PINO_UART_EQUIPE_TX = GPIO_NUM_17;
constexpr gpio_num_t PINO_UART_EQUIPE_RX = GPIO_NUM_18;

// Comunicação com o módulo de visitantes.
constexpr uart_port_t UART_VISITANTES        = UART_NUM_2;
constexpr gpio_num_t PINO_UART_VISITANTES_TX = GPIO_NUM_15;
constexpr gpio_num_t PINO_UART_VISITANTES_RX = GPIO_NUM_16;

// Cartão SD
// SPI2 sugerido para o ESP32-S3-WROOM-1-N16R8. Confirme estes GPIOs na montagem.
constexpr gpio_num_t PINO_SD_CLK    = GPIO_NUM_38;
constexpr gpio_num_t PINO_SD_MOSI   = GPIO_NUM_39;
constexpr gpio_num_t PINO_SD_MISO   = GPIO_NUM_40;
constexpr gpio_num_t PINO_SD_CS     = GPIO_NUM_41;
constexpr uint32_t FREQUENCIA_SD_HZ             = 10U * 1000U * 1000U;
constexpr uint32_t FREQUENCIA_SD_ALTERNATIVA_HZ = 400U * 1000U;

}  // namespace configuracao
