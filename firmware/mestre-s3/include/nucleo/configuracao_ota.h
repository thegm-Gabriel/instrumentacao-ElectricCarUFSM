#pragma once

#include <cstddef>
#include <cstdint>

/*
 * Copie segredos_ota.exemplo.h para segredos_ota.h e preencha a rede usada pelo
 * mestre. O arquivo real fica fora do Git para não publicar a senha.
 */
#if __has_include("nucleo/segredos_ota.h")
#include "nucleo/segredos_ota.h"
#else
#define OTA_WIFI_SSID ""
#define OTA_WIFI_SENHA ""
#endif

namespace configuracao {

constexpr bool HABILITAR_OTA = true;
constexpr char WIFI_OTA_SSID[] = OTA_WIFI_SSID;
constexpr char WIFI_OTA_SENHA[] = OTA_WIFI_SENHA;

constexpr char REPOSITORIO_GITHUB[] =
    "thegm-Gabriel/instrumentacao-ElectricCarUFSM";
constexpr char ENDERECO_MANIFESTO_OTA[] =
    "https://github.com/thegm-Gabriel/instrumentacao-ElectricCarUFSM/"
    "releases/latest/download/manifesto-mestre.json";

constexpr uint32_t TEMPO_LIMITE_CONEXAO_WIFI_MS = 30000;
constexpr uint32_t TEMPO_LIMITE_HTTP_MS = 20000;
constexpr uint32_t INTERVALO_VERIFICACAO_OTA_MS = 15 * 60 * 1000;
constexpr int MAXIMO_TENTATIVAS_WIFI = 10;
constexpr std::size_t TAMANHO_MAXIMO_MANIFESTO_OTA = 2048;

}  // namespace configuracao
