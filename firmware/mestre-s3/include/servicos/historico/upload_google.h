#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

struct ObservadorUploadGoogle {
    void (*iniciar)(const char* nome, std::size_t tamanho);
    void (*atualizar)(uint32_t enviados, uint32_t velocidade_bytes_s,
                      uint32_t tempo_restante_s);
    void (*aguardar_confirmacao)(bool aguardando);
};

// Envia um CSV fechado e confirma o recebimento pelo nome.
esp_err_t upload_google_enviar(const char* caminho, const char* nome,
                              uint32_t indice, uint32_t quantidade,
                              uint32_t* tamanho_enviado,
                              const ObservadorUploadGoogle& observador);
