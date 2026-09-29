#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "gerenciadores/gerenciador_uart.h"

struct ConfiguracaoTransferenciaOtaSatelite {
    DestinoUart destino = DestinoUart::Equipe;
    const char* endereco_https = nullptr;
    const char* versao = nullptr;
    const char* sha256 = nullptr;
    uint32_t tamanho_bytes = 0;
    uint32_t tempo_limite_http_ms = 20000;
};

using ObservadorProgressoOtaSatelite = void (*)(uint32_t bytes_enviados,
                                                 uint32_t tamanho_total,
                                                 uint32_t tempo_decorrido_ms);

esp_err_t gerenciador_ota_satelites_iniciar();
esp_err_t gerenciador_ota_satelites_executar(
    const ConfiguracaoTransferenciaOtaSatelite& configuracao,
    ObservadorProgressoOtaSatelite observador);
void gerenciador_ota_satelites_registrar_resposta(DestinoUart origem,
                                                  const uint8_t* quadro,
                                                  size_t tamanho);

