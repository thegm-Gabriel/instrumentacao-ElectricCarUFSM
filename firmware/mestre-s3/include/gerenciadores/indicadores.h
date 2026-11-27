// Declara o controle físico dos LEDs e do buzzer.
#pragma once

#include <cstdint>

#include "esp_err.h"

struct CorLed {
    uint8_t vermelho = 0;
    uint8_t verde = 0;
    uint8_t azul = 0;

    constexpr bool operator==(const CorLed& outra) const {
        return vermelho == outra.vermelho && verde == outra.verde && azul == outra.azul;
    }
};

struct EstadoLeds {
    bool verde = false;
    bool amarelo = false;
    bool vermelho = false;
    CorLed integrado{};

    constexpr bool operator==(const EstadoLeds& outro) const {
        return verde == outro.verde && amarelo == outro.amarelo &&
            vermelho == outro.vermelho && integrado == outro.integrado;
    }
};

// Controla somente o hardware. A escolha das cores e melodias pertence ao serviço.
esp_err_t gerenciador_indicadores_iniciar();
esp_err_t gerenciador_indicadores_definir_leds(const EstadoLeds& estado);
esp_err_t gerenciador_indicadores_tocar_tom(uint16_t frequencia_hz,
                                           uint8_t intensidade_percentual);
esp_err_t gerenciador_indicadores_silenciar();
