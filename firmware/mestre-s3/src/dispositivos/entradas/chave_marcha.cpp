#include "dispositivos/entradas/chave_marcha.h"

#include "driver/gpio.h"
#include "nucleo/configuracao/placa.h"
#include "controle_remoto.h"

esp_err_t chave_marcha_iniciar() {
    if (!configuracao::HABILITAR_LEITURA_CHAVE_MARCHA) return ESP_OK;
    const gpio_num_t pinos[] = {
        configuracao::PINO_CHAVE_MARCHA_L,
        configuracao::PINO_CHAVE_MARCHA_H,
        configuracao::PINO_CHAVE_REVERSAO,
    };
    gpio_config_t config{};
    for (const gpio_num_t pino : pinos) {
        if (!GPIO_IS_VALID_GPIO(pino) || (config.pin_bit_mask & (1ULL << pino)))
            return ESP_ERR_INVALID_ARG;
        config.pin_bit_mask |= 1ULL << static_cast<unsigned>(pino);
    }
    config.mode = GPIO_MODE_INPUT;
    // Os fios requerem condicionamento e pull-ups externos para 3,3 V.
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    return gpio_config(&config);
}

esp_err_t chave_marcha_ler(uint8_t* marcha) {
    if (marcha == nullptr) return ESP_ERR_INVALID_ARG;
    *marcha = FAIXA_MARCHA_INDEFINIDA;
    if (!configuracao::HABILITAR_LEITURA_CHAVE_MARCHA)
        return ESP_ERR_INVALID_STATE;
    const int l = gpio_get_level(configuracao::PINO_CHAVE_MARCHA_L);
    const int h = gpio_get_level(configuracao::PINO_CHAVE_MARCHA_H);
    if (l == 0 && h == 0) return ESP_ERR_INVALID_RESPONSE;
    *marcha = l == 0 ? FAIXA_MARCHA_L :
              h == 0 ? FAIXA_MARCHA_H : FAIXA_MARCHA_M;
    return ESP_OK;
}

bool chave_marcha_re_selecionada() {
    return configuracao::HABILITAR_LEITURA_CHAVE_MARCHA &&
        gpio_get_level(configuracao::PINO_CHAVE_REVERSAO) == 0;
}
