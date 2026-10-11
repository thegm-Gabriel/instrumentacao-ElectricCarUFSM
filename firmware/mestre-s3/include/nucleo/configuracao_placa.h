// Centraliza pinagem e limites físicos da placa mestre.
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

// Montagem atual: X vertical (+ para cima) e Y longitudinal (+ para frente).
// Use 0/1/2 para X/Y/Z e troque o sinal para -1,0 se a orientação mudar.
constexpr uint8_t EIXO_LONGITUDINAL_MPU6050 = 1;
constexpr float SINAL_EIXO_LONGITUDINAL_MPU6050 = 1.0f;
constexpr uint32_t FREQUENCIA_AMOSTRAGEM_MPU6050_HZ = 100;
constexpr float FREQUENCIA_CORTE_ACELERACAO_HZ = 3.0f;
constexpr uint16_t AMOSTRAS_CALIBRACAO_MPU6050 = 100;
constexpr float LIMITE_GIRO_CALIBRACAO_MPU6050_GRAUS_S = 5.0f;
constexpr float GRAVIDADE_MINIMA_CALIBRACAO_MPU6050_G = 0.85f;
constexpr float GRAVIDADE_MAXIMA_CALIBRACAO_MPU6050_G = 1.15f;
// Impede calibrar como longitudinal um eixo praticamente vertical. Ajuste o
// eixo X/Y/Z acima conforme a montagem se esta validação rejeitar a calibração.
constexpr float GRAVIDADE_MAXIMA_EIXO_LONGITUDINAL_G = 0.50f;
// O giroscópio acompanha rapidamente a orientação; o acelerômetro corrige
// lentamente a deriva apenas quando a aceleração linear estimada está baixa.
constexpr float FREQUENCIA_CORRECAO_GRAVIDADE_HZ = 0.05f;
constexpr float LIMITE_ACELERACAO_CORRECAO_GRAVIDADE_G = 0.012f;
// Histerese e confirmação evitam inversões rápidas causadas por vibração.
constexpr float LIMIAR_ENTRADA_SENTIDO_ACELERACAO_G = 0.03f;
constexpr float LIMIAR_SAIDA_SENTIDO_ACELERACAO_G = 0.015f;
constexpr uint32_t TEMPO_CONFIRMACAO_SENTIDO_ACELERACAO_MS = 80;
// Frequência apenas da apresentação no terminal; não altera a aquisição.
constexpr uint32_t INTERVALO_MONITOR_SENSORES_MS = 2000;

// Encoders incrementais E38S6G5-600B-G24N. As entradas A/B exigem pull-ups
// externos para 3,3 V; nunca aplique 12/24 V aos GPIOs do ESP32-S3.
constexpr gpio_num_t PINO_ENCODER_ESQUERDO_A = GPIO_NUM_5;
constexpr gpio_num_t PINO_ENCODER_ESQUERDO_B = GPIO_NUM_6;
constexpr gpio_num_t PINO_ENCODER_DIREITO_A = GPIO_NUM_7;
constexpr gpio_num_t PINO_ENCODER_DIREITO_B = GPIO_NUM_21;
constexpr uint32_t PULSOS_ENCODER_ESQUERDO_POR_GIRO = 600;
constexpr uint32_t PULSOS_ENCODER_DIREITO_POR_GIRO = 600;
constexpr uint32_t FILTRO_RUIDO_ENCODER_NS = 1000;

// Ajuste cada lado conforme a montagem mecânica. A relação representa quantos
// giros o encoder completa para cada giro da roda (redução 4:1 = 4,0).
constexpr float DIAMETRO_RODA_ESQUERDA_M = 0.300f;
constexpr float DIAMETRO_RODA_DIREITA_M = 0.300f;
constexpr float GIROS_ENCODER_ESQUERDO_POR_GIRO_RODA = 1.0f;
constexpr float GIROS_ENCODER_DIREITO_POR_GIRO_RODA = 1.0f;

// Distância entre gravações do total na NVS. Aumentar reduz o desgaste da flash
// e aumenta a distância que pode ser perdida em uma queda súbita de energia.
constexpr float INTERVALO_PERSISTENCIA_ODOMETRO_M = 100.0f;

// Limites usados apenas para diagnosticar leituras fisicamente improváveis.
// A distância continua sendo a média das duas rodas.
constexpr float VELOCIDADE_MAXIMA_ODOMETRO_KMH = 200.0f;
constexpr float RAZAO_MINIMA_MOVIMENTO_ENTRE_RODAS = 0.20f;
constexpr float MOVIMENTO_MINIMO_DIAGNOSTICO_RODAS_M = 0.05f;
constexpr uint16_t AMOSTRAS_DIVERGENTES_PARA_ALERTA = 10;

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
constexpr std::size_t TAMANHO_MAXIMO_MENSAGEM_UART = 4224;
constexpr std::size_t TAMANHO_MAXIMO_MENSAGEM_UART_ASSINCRONA = 256;
constexpr std::size_t CAPACIDADE_FILA_UART_TX = 4;
// Interfaces da equipe e visitantes recebem quatro atualizações por segundo.
constexpr int INTERVALO_TELEMETRIA_MS = 250;
constexpr uint32_t INTERVALO_TELEMETRIA_OTA_PADRAO_MS = 2000;
constexpr uint32_t INTERVALO_TELEMETRIA_OTA_MINIMO_MS = 500;
constexpr uint32_t INTERVALO_TELEMETRIA_OTA_MAXIMO_MS = 10000;
constexpr bool USAR_TELEMETRIA_SIMULADA = false;
constexpr uint16_t DINAMICA_SIMULACAO_PERCENTUAL = 100;

}  // namespace configuracao
