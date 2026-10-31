// Grava amostras WAV temporárias para treinar o reconhecedor de comandos.
#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

struct ResultadoGravacaoAudioTreinamento {
    char caminho[128] = {};
    uint32_t amostras_gravadas = 0;
    uint32_t duracao_ms = 0;
    uint32_t amostras_saturadas = 0;
    int16_t pico_absoluto = 0;
    bool arquivo_existente = false;
    bool arquivo_parcial = false;
};

// Cria /sdcard/audio_ia/<nome>.wav em PCM mono de 16 bits e 16 kHz.
// O nome é normalizado para letras, números, '-' e '_'. Arquivos existentes
// nunca são sobrescritos.
esp_err_t servico_gravador_audio_treinamento_gravar(
    const char* nome, uint32_t duracao_segundos,
    ResultadoGravacaoAudioTreinamento* resultado);
