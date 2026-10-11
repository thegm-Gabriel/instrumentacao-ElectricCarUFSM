// Declara o transporte confirmado de firmware aos satélites.
#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "nucleo/destino_enlace.h"
#include "protocolo_ota_satelites.h"

struct ConfiguracaoTransferenciaOtaSatelite {
    DestinoUart destino = DestinoUart::Equipe;
    const char* endereco_https = nullptr;
    const char* versao = nullptr;
    const char* sha256 = nullptr;
    uint32_t tamanho_bytes = 0;
    uint16_t tamanho_bloco = OTA_SATELITE_TAMANHO_CARGA_COMPATIVEL;
    uint32_t tempo_limite_http_ms = 20000;
};

using ObservadorProgressoOtaSatelite = void (*)(uint32_t bytes_enviados,
                                                 uint32_t tamanho_total,
                                                 uint32_t tempo_decorrido_ms);

esp_err_t transporte_ota_satelites_iniciar();
esp_err_t transporte_ota_satelites_encerrar();
esp_err_t transporte_ota_satelites_executar(
    const ConfiguracaoTransferenciaOtaSatelite& configuracao,
    ObservadorProgressoOtaSatelite observador);
void transporte_ota_satelites_registrar_resposta(DestinoUart origem,
                                                  const uint8_t* quadro,
                                                  size_t tamanho);
