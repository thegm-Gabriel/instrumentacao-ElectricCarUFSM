#pragma once

#include <cstdint>

#include "esp_err.h"

struct ConfiguracaoOta {
    const char* endereco_https = nullptr;
    const char* certificado_raiz = nullptr;
    uint32_t tempo_limite_ms = 15000;
};

esp_err_t gerenciador_ota_iniciar(const ConfiguracaoOta& configuracao);
esp_err_t gerenciador_ota_executar();
bool gerenciador_ota_esta_configurado();
