// Declara o controle físico dos LEDs e do buzzer.
#pragma once

#include <cstdint>

#include "esp_err.h"

struct EstadoLeds {
    bool verde = false;
    bool amarelo = false;
    bool vermelho = false;
    bool azul = false;
};

// Controla somente o hardware. A escolha das cores e melodias pertence ao serviço.
esp_err_t gerenciador_indicadores_iniciar();
esp_err_t gerenciador_indicadores_definir_leds(const EstadoLeds& estado);
esp_err_t gerenciador_indicadores_tocar_tom(uint16_t frequencia_hz,
                                           uint8_t intensidade_percentual);
esp_err_t gerenciador_indicadores_silenciar();
