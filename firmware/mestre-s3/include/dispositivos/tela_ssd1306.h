#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

struct EstadoTelaSsd1306 {
    bool iniciada = false;
    bool presente = false;
    uint8_t endereco_i2c = 0;
    uint32_t quadros_enviados = 0;
    uint32_t quadros_inalterados = 0;
    uint32_t erros = 0;
    esp_err_t ultimo_erro = ESP_OK;
};

esp_err_t tela_ssd1306_iniciar();
// Copia e desenha imediatamente até oito linhas ASCII de 21 caracteres.
esp_err_t tela_ssd1306_exibir_linhas(const char* const* linhas,
                                     std::size_t quantidade_linhas);
// Exibe até sete linhas e usa a última faixa da tela como barra de 0,0 a 100,0%.
esp_err_t tela_ssd1306_exibir_linhas_com_progresso(
    const char* const* linhas, std::size_t quantidade_linhas,
    uint16_t percentual_decimos);
esp_err_t tela_ssd1306_limpar();
EstadoTelaSsd1306 tela_ssd1306_obter_estado();
