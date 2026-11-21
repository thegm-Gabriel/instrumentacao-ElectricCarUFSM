// Buffers UART, prazos de enlace e cadência da telemetria.
#pragma once

#include <cstddef>
#include <cstdint>

#include "configuracao_enlace_uart.h"

namespace configuracao {

// UART
// A velocidade segue o protocolo compartilhado com os satélites.
constexpr int VELOCIDADE_UART                                 = ENLACE_UART_BAUD_RATE;
constexpr int TAMANHO_BUFFER_UART_RX                          = 8192;
constexpr int TAMANHO_BUFFER_UART_TX                          = 8192;
constexpr std::size_t TAMANHO_MAXIMO_MENSAGEM_UART            = 4224;
constexpr std::size_t TAMANHO_MAXIMO_MENSAGEM_UART_ASSINCRONA = 256;
constexpr std::size_t CAPACIDADE_FILA_UART_TX                 = 4;

// Telemetria ao vivo
// Intervalos do envio em condições normais e durante OTA.
constexpr int INTERVALO_TELEMETRIA_MS                 = 250;
constexpr uint32_t INTERVALO_TELEMETRIA_OTA_PADRAO_MS = 2000;
constexpr uint32_t INTERVALO_TELEMETRIA_OTA_MINIMO_MS = 500;
constexpr uint32_t INTERVALO_TELEMETRIA_OTA_MAXIMO_MS = 10000;

// Recepção UART
// Margem de supervisão e prazo para descartar um quadro incompleto.
constexpr uint32_t MARGEM_SUPERVISAO_UART_MS     = 5000;
constexpr int64_t TEMPO_LIMITE_QUADRO_PARCIAL_US = 250000;

// Número máximo de tentativas para uma consulta UART.
constexpr unsigned MAXIMO_TENTATIVAS_COMANDO_UART = 3;

}  // namespace configuracao
