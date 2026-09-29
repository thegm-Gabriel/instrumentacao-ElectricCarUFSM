#pragma once

#include <cstdint>

#include "esp_err.h"

struct EstadoLeds {
    bool verde = false;
    bool amarelo = false;
    bool vermelho = false;
    bool azul = false;
};

struct EstatisticasIndicadores {
    bool iniciado = false;
    EstadoLeds leds{};
    bool buzzer_ativo = false;
    uint16_t frequencia_buzzer_hz = 0;
    uint32_t alteracoes_leds = 0;
    uint32_t tons_reproduzidos = 0;
    uint32_t erros = 0;
    esp_err_t ultimo_erro = ESP_OK;
};

// Controla somente o hardware. A escolha das cores e melodias pertence ao serviço.
esp_err_t gerenciador_indicadores_iniciar();
esp_err_t gerenciador_indicadores_definir_leds(const EstadoLeds& estado);
esp_err_t gerenciador_indicadores_tocar_tom(uint16_t frequencia_hz,
                                           uint8_t intensidade_percentual);
esp_err_t gerenciador_indicadores_silenciar();
EstatisticasIndicadores gerenciador_indicadores_obter_estatisticas();
