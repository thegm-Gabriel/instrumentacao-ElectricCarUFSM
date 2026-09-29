#pragma once

#include <cstdint>

#include "esp_err.h"

using ObservadorProgressoOta = void (*)(uint32_t bytes_recebidos,
                                        uint32_t tamanho_total,
                                        uint32_t tempo_decorrido_ms);

struct ConfiguracaoOta {
    const char* endereco_https = nullptr;
    const char* versao_esperada = nullptr;
    const char* sha256_esperado = nullptr;
    const char* certificado_raiz = nullptr;
    uint32_t tempo_limite_ms = 15000;
    uint32_t tamanho_esperado = 0;
    int tamanho_buffer_http_tx = 4096;
    ObservadorProgressoOta observador_progresso = nullptr;
};

esp_err_t gerenciador_ota_iniciar(const ConfiguracaoOta& configuracao);
bool gerenciador_ota_sha256_valido(const char* texto);
esp_err_t gerenciador_ota_executar();
esp_err_t gerenciador_ota_confirmar_firmware_em_execucao();
esp_err_t gerenciador_ota_rejeitar_firmware_em_execucao();
