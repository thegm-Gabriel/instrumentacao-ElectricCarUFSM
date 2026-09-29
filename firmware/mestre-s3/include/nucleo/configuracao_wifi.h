#pragma once

#include <cstddef>
#include <cstdint>

/*
 * Copie segredos_ota.exemplo.h para segredos_ota.h. O mesmo arquivo privado
 * concentra as credenciais de rede e permanece fora do Git.
 */
#if __has_include("nucleo/segredos_ota.h")
#include "nucleo/segredos_ota.h"
#else
#define OTA_WIFI_SSID ""
#define OTA_WIFI_SENHA ""
#endif

#ifndef WIFI_SENHA_PERMISSAO
#define WIFI_SENHA_PERMISSAO OTA_WIFI_SENHA
#endif

#ifndef WIFI_CANAL_PREFERENCIAL
#define WIFI_CANAL_PREFERENCIAL 0
#endif
#ifndef WIFI_LARGURA_CANAL_MHZ
#define WIFI_LARGURA_CANAL_MHZ 20
#endif
#ifndef WIFI_POTENCIA_MAXIMA_DBM
#define WIFI_POTENCIA_MAXIMA_DBM 16
#endif

namespace configuracao {

constexpr char WIFI_PRINCIPAL_SSID[] = OTA_WIFI_SSID;
constexpr char WIFI_PRINCIPAL_SENHA[] = OTA_WIFI_SENHA;
constexpr char SENHA_PERMISSAO_WIFI[] = WIFI_SENHA_PERMISSAO;
constexpr std::size_t MAXIMO_REDES_WIFI_USUARIO = 5;
constexpr uint8_t CANAL_WIFI_PREFERENCIAL = WIFI_CANAL_PREFERENCIAL;
constexpr uint8_t LARGURA_CANAL_WIFI_MHZ = WIFI_LARGURA_CANAL_MHZ;
constexpr int8_t POTENCIA_MAXIMA_WIFI_DBM = WIFI_POTENCIA_MAXIMA_DBM;

static_assert(CANAL_WIFI_PREFERENCIAL <= 13,
              "O canal Wi-Fi preferencial deve estar entre 0 e 13");
static_assert(LARGURA_CANAL_WIFI_MHZ == 20 ||
                  LARGURA_CANAL_WIFI_MHZ == 40,
              "A largura do canal Wi-Fi deve ser 20 ou 40 MHz");
static_assert(POTENCIA_MAXIMA_WIFI_DBM >= 2 &&
                  POTENCIA_MAXIMA_WIFI_DBM <= 20,
              "A potência Wi-Fi deve estar entre 2 e 20 dBm");

constexpr uint8_t TENTATIVAS_WIFI_PADRAO = 3;
constexpr uint8_t TENTATIVAS_WIFI_MINIMAS = 1;
constexpr uint8_t TENTATIVAS_WIFI_MAXIMAS = 10;
constexpr uint32_t TEMPO_TENTATIVA_WIFI_PADRAO_MS = 8000;
constexpr uint32_t TEMPO_TENTATIVA_WIFI_MINIMO_MS = 2000;
constexpr uint32_t TEMPO_TENTATIVA_WIFI_MAXIMO_MS = 30000;
constexpr uint32_t INTERVALO_TENTATIVAS_WIFI_PADRAO_MS = 750;
constexpr uint32_t INTERVALO_TENTATIVAS_WIFI_MINIMO_MS = 100;
constexpr uint32_t INTERVALO_TENTATIVAS_WIFI_MAXIMO_MS = 5000;
constexpr uint32_t TEMPO_LIMITE_SESSAO_WIFI_PADRAO_MS = 30000;
constexpr uint32_t TEMPO_LIMITE_SESSAO_WIFI_MINIMO_MS = 5000;
constexpr uint32_t TEMPO_LIMITE_SESSAO_WIFI_MAXIMO_MS = 180000;

}  // namespace configuracao
