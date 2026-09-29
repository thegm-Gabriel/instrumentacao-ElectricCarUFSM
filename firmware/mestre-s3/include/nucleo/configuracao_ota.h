#pragma once

#include <cstddef>
#include <cstdint>

namespace configuracao {

constexpr bool HABILITAR_OTA = true;
constexpr char ENDERECO_MANIFESTO_OTA[] =
    "https://github.com/thegm-Gabriel/instrumentacao-ElectricCarUFSM/"
    "releases/latest/download/manifesto-firmwares.json";

constexpr uint32_t TEMPO_LIMITE_HTTP_MS = 20000;
constexpr int TAMANHO_BUFFER_HTTP_TX = 4096;
constexpr uint32_t INTERVALO_VERIFICACAO_OTA_PADRAO_MIN = 15;
constexpr uint32_t INTERVALO_VERIFICACAO_OTA_MINIMO_MIN = 1;
constexpr uint32_t INTERVALO_VERIFICACAO_OTA_MAXIMO_MIN = 7 * 24 * 60;
constexpr std::size_t TAMANHO_MAXIMO_MANIFESTO_OTA = 8192;

}  // namespace configuracao
