#include "gerenciadores/gerenciador_indicadores.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nucleo/configuracao_placa.h"

namespace {
constexpr char ETIQUETA[] = "indicadores";
constexpr uint16_t FREQUENCIA_MINIMA_HZ = 50;
constexpr uint16_t FREQUENCIA_MAXIMA_HZ = 10000;
constexpr uint32_t DUTY_MAXIMO_10_BITS = 1023;
constexpr uint32_t DUTY_ONDA_QUADRADA = (DUTY_MAXIMO_10_BITS + 1U) / 2U;

SemaphoreHandle_t mutex_indicadores = nullptr;
EstatisticasIndicadores estatisticas{};

int nivel_led(bool aceso) {
    const bool nivel_alto = configuracao::LEDS_ATIVOS_EM_NIVEL_ALTO ? aceso : !aceso;
    return nivel_alto ? 1 : 0;
}

void registrar_erro(esp_err_t erro) {
    if (erro == ESP_OK) return;
    estatisticas.erros++;
    estatisticas.ultimo_erro = erro;
}

esp_err_t escrever_led(gpio_num_t pino, bool aceso) {
    return gpio_set_level(pino, nivel_led(aceso));
}
}  // namespace

esp_err_t gerenciador_indicadores_iniciar() {
    if (estatisticas.iniciado) return ESP_OK;
    if (mutex_indicadores == nullptr) {
        mutex_indicadores = xSemaphoreCreateMutex();
        if (mutex_indicadores == nullptr) return ESP_ERR_NO_MEM;
    }

    const uint64_t mascara_leds =
        (1ULL << configuracao::PINO_LED_VERDE) |
        (1ULL << configuracao::PINO_LED_AMARELO) |
        (1ULL << configuracao::PINO_LED_VERMELHO) |
        (1ULL << configuracao::PINO_LED_AZUL);
    gpio_config_t gpio_leds{};
    gpio_leds.pin_bit_mask = mascara_leds;
    gpio_leds.mode = GPIO_MODE_OUTPUT;
    gpio_leds.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_leds.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_leds.intr_type = GPIO_INTR_DISABLE;

    esp_err_t erro = gpio_config(&gpio_leds);
    if (erro != ESP_OK) {
        registrar_erro(erro);
        ESP_LOGE(ETIQUETA, "Falha ao configurar os LEDs: %s", esp_err_to_name(erro));
        return erro;
    }
    erro = escrever_led(configuracao::PINO_LED_VERDE, false);
    if (erro == ESP_OK) erro = escrever_led(configuracao::PINO_LED_AMARELO, false);
    if (erro == ESP_OK) erro = escrever_led(configuracao::PINO_LED_VERMELHO, false);
    if (erro == ESP_OK) erro = escrever_led(configuracao::PINO_LED_AZUL, false);
    if (erro != ESP_OK) {
        registrar_erro(erro);
        ESP_LOGE(ETIQUETA, "Falha ao apagar os LEDs na inicialização: %s",
                 esp_err_to_name(erro));
        return erro;
    }

    ledc_timer_config_t temporizador{};
    temporizador.speed_mode = configuracao::MODO_PWM_BUZZER;
    temporizador.duty_resolution = configuracao::RESOLUCAO_PWM_BUZZER;
    temporizador.timer_num = configuracao::TEMPORIZADOR_PWM_BUZZER;
    temporizador.freq_hz = configuracao::FREQUENCIA_INICIAL_BUZZER_HZ;
    temporizador.clk_cfg = LEDC_AUTO_CLK;
    erro = ledc_timer_config(&temporizador);
    if (erro != ESP_OK) {
        registrar_erro(erro);
        ESP_LOGE(ETIQUETA, "Falha ao configurar o PWM do buzzer: %s",
                 esp_err_to_name(erro));
        return erro;
    }

    ledc_channel_config_t canal{};
    canal.gpio_num = configuracao::PINO_BUZZER_PASSIVO;
    canal.speed_mode = configuracao::MODO_PWM_BUZZER;
    canal.channel = configuracao::CANAL_PWM_BUZZER;
    canal.intr_type = LEDC_INTR_DISABLE;
    canal.timer_sel = configuracao::TEMPORIZADOR_PWM_BUZZER;
    canal.duty = 0;
    canal.hpoint = 0;
    erro = ledc_channel_config(&canal);
    if (erro != ESP_OK) {
        registrar_erro(erro);
        ESP_LOGE(ETIQUETA, "Falha ao associar o buzzer ao PWM: %s",
                 esp_err_to_name(erro));
        return erro;
    }

    estatisticas.iniciado = true;
    estatisticas.ultimo_erro = ESP_OK;
    ESP_LOGI(ETIQUETA,
             "Indicadores prontos: LEDs V/A/Vm/Az=%d/%d/%d/%d | buzzer passivo GPIO %d",
             configuracao::PINO_LED_VERDE, configuracao::PINO_LED_AMARELO,
             configuracao::PINO_LED_VERMELHO, configuracao::PINO_LED_AZUL,
             configuracao::PINO_BUZZER_PASSIVO);
    return ESP_OK;
}

esp_err_t gerenciador_indicadores_definir_leds(const EstadoLeds& estado) {
    if (!estatisticas.iniciado || mutex_indicadores == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(mutex_indicadores, portMAX_DELAY);
    esp_err_t erro = escrever_led(configuracao::PINO_LED_VERDE, estado.verde);
    if (erro == ESP_OK) erro = escrever_led(configuracao::PINO_LED_AMARELO, estado.amarelo);
    if (erro == ESP_OK) erro = escrever_led(configuracao::PINO_LED_VERMELHO, estado.vermelho);
    if (erro == ESP_OK) erro = escrever_led(configuracao::PINO_LED_AZUL, estado.azul);
    if (erro == ESP_OK) {
        estatisticas.leds = estado;
        estatisticas.alteracoes_leds++;
        estatisticas.ultimo_erro = ESP_OK;
    } else {
        registrar_erro(erro);
    }
    xSemaphoreGive(mutex_indicadores);
    return erro;
}

esp_err_t gerenciador_indicadores_tocar_tom(uint16_t frequencia_hz,
                                           uint8_t intensidade_percentual) {
    if (!estatisticas.iniciado || mutex_indicadores == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (frequencia_hz < FREQUENCIA_MINIMA_HZ ||
        frequencia_hz > FREQUENCIA_MAXIMA_HZ || intensidade_percentual > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (intensidade_percentual == 0) return gerenciador_indicadores_silenciar();

    xSemaphoreTake(mutex_indicadores, portMAX_DELAY);
    esp_err_t erro = ledc_set_freq(configuracao::MODO_PWM_BUZZER,
                                   configuracao::TEMPORIZADOR_PWM_BUZZER,
                                   frequencia_hz);
    const uint32_t duty =
        (DUTY_ONDA_QUADRADA * static_cast<uint32_t>(intensidade_percentual)) / 100U;
    if (erro == ESP_OK) {
        erro = ledc_set_duty(configuracao::MODO_PWM_BUZZER,
                             configuracao::CANAL_PWM_BUZZER, duty);
    }
    if (erro == ESP_OK) {
        erro = ledc_update_duty(configuracao::MODO_PWM_BUZZER,
                                configuracao::CANAL_PWM_BUZZER);
    }
    if (erro == ESP_OK) {
        estatisticas.buzzer_ativo = true;
        estatisticas.frequencia_buzzer_hz = frequencia_hz;
        estatisticas.tons_reproduzidos++;
        estatisticas.ultimo_erro = ESP_OK;
    } else {
        registrar_erro(erro);
    }
    xSemaphoreGive(mutex_indicadores);
    return erro;
}

esp_err_t gerenciador_indicadores_silenciar() {
    if (!estatisticas.iniciado || mutex_indicadores == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(mutex_indicadores, portMAX_DELAY);
    esp_err_t erro = ledc_set_duty(configuracao::MODO_PWM_BUZZER,
                                   configuracao::CANAL_PWM_BUZZER, 0);
    if (erro == ESP_OK) {
        erro = ledc_update_duty(configuracao::MODO_PWM_BUZZER,
                                configuracao::CANAL_PWM_BUZZER);
    }
    if (erro == ESP_OK) {
        estatisticas.buzzer_ativo = false;
        estatisticas.frequencia_buzzer_hz = 0;
        estatisticas.ultimo_erro = ESP_OK;
    } else {
        registrar_erro(erro);
    }
    xSemaphoreGive(mutex_indicadores);
    return erro;
}

EstatisticasIndicadores gerenciador_indicadores_obter_estatisticas() {
    if (mutex_indicadores == nullptr) return estatisticas;
    xSemaphoreTake(mutex_indicadores, portMAX_DELAY);
    const EstatisticasIndicadores copia = estatisticas;
    xSemaphoreGive(mutex_indicadores);
    return copia;
}
