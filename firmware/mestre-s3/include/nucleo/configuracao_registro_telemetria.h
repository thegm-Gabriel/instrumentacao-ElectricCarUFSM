// Centraliza a taxa de registro, a rotação do CSV, o cartão SD e o destino Google.
#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"

#if __has_include("nucleo/segredos_ota.h")
#include "nucleo/segredos_ota.h"
#endif

#ifndef TELEMETRIA_GOOGLE_URL
#define TELEMETRIA_GOOGLE_URL ""
#endif

#ifndef TELEMETRIA_GOOGLE_TOKEN
#define TELEMETRIA_GOOGLE_TOKEN ""
#endif

namespace configuracao {

// O registro é degradável: a ausência do cartão não interrompe a telemetria ao vivo.
constexpr bool HABILITAR_REGISTRO_TELEMETRIA = true;
constexpr uint32_t INTERVALO_REGISTRO_TELEMETRIA_MS = 1000;
constexpr uint32_t INTERVALO_SINCRONIZACAO_SD_MS = 10000;
constexpr uint32_t DURACAO_ARQUIVO_TELEMETRIA_MS = 5U * 60U * 1000U;
constexpr std::size_t CAPACIDADE_FILA_REGISTRO = 24;
constexpr std::size_t TAMANHO_MAXIMO_CSV_BYTES = 1536U * 1024U;

// SPI2 sugerido para o ESP32-S3-WROOM-1-N16R8. Confirme estes GPIOs na montagem.
constexpr gpio_num_t PINO_SD_CLK = GPIO_NUM_38;
constexpr gpio_num_t PINO_SD_MOSI = GPIO_NUM_39;
constexpr gpio_num_t PINO_SD_MISO = GPIO_NUM_40;
constexpr gpio_num_t PINO_SD_CS = GPIO_NUM_41;
constexpr uint32_t FREQUENCIA_SD_HZ = 10U * 1000U * 1000U;
constexpr char PONTO_MONTAGEM_SD[] = "/sdcard";
constexpr char DIRETORIO_TELEMETRIA[] = "/sdcard/telemetria";
constexpr char DIRETORIO_TELEMETRIA_ENVIADA[] = "/sdcard/telemetria/enviados";

constexpr char ENDERECO_GOOGLE_TELEMETRIA[] = TELEMETRIA_GOOGLE_URL;
constexpr char TOKEN_GOOGLE_TELEMETRIA[] = TELEMETRIA_GOOGLE_TOKEN;
constexpr uint32_t TEMPO_LIMITE_UPLOAD_MS = 60000;
constexpr uint8_t MAXIMO_TENTATIVAS_UPLOAD = 4;
constexpr uint32_t ESPERA_INICIAL_NOVA_TENTATIVA_MS = 1000;

static_assert(INTERVALO_REGISTRO_TELEMETRIA_MS >= 1000,
              "O histórico no Google foi definido para no máximo 1 Hz");
static_assert(DURACAO_ARQUIVO_TELEMETRIA_MS >=
                  INTERVALO_REGISTRO_TELEMETRIA_MS,
              "A duração do CSV deve comportar pelo menos uma amostra");

}  // namespace configuracao
