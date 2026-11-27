// Implementa acionamento seguro de LEDs e buzzer.
#include "gerenciadores/indicadores.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nucleo/configuracao/placa.h"

namespace {
constexpr char ETIQUETA[] = "indicadores";
constexpr uint16_t FREQUENCIA_MINIMA_HZ = 50;
constexpr uint16_t FREQUENCIA_MAXIMA_HZ = 10000;
constexpr uint32_t DUTY_MAXIMO_10_BITS = 1023;
constexpr uint32_t DUTY_ONDA_QUADRADA = (DUTY_MAXIMO_10_BITS + 1U) / 2U;

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

SemaphoreHandle_t mutex_indicadores = nullptr;
EstatisticasIndicadores estatisticas{};
rmt_channel_handle_t canal_led_integrado = nullptr;
rmt_encoder_handle_t codificador_led_integrado = nullptr;
// Um pixel GRB e o pulso de reset cabem em um bloco do ESP32-S3.
// O buffer deve permanecer válido até o RMT concluir a transmissão.
rmt_symbol_word_t quadro_led_integrado[25]{};
CorLed ultima_cor_led_integrado{};
bool cor_led_integrado_enviada = false;

esp_err_t iniciar_led_integrado() {
    rmt_tx_channel_config_t canal{};
    canal.gpio_num = configuracao::PINO_LED_INTEGRADO;
    canal.clk_src = RMT_CLK_SRC_DEFAULT;
    canal.resolution_hz = 10000000;
    canal.mem_block_symbols = 48;
    canal.trans_queue_depth = 1;
    esp_err_t erro = rmt_new_tx_channel(&canal, &canal_led_integrado);
    if (erro != ESP_OK) return erro;

    const rmt_copy_encoder_config_t copia{};
    erro = rmt_new_copy_encoder(&copia, &codificador_led_integrado);
    if (erro == ESP_OK) erro = rmt_enable(canal_led_integrado);
    if (erro != ESP_OK) {
        if (codificador_led_integrado != nullptr) rmt_del_encoder(codificador_led_integrado);
        rmt_del_channel(canal_led_integrado);
        codificador_led_integrado = nullptr;
        canal_led_integrado = nullptr;
    }
    return erro;
}

esp_err_t escrever_led_integrado(CorLed cor) {
    if (canal_led_integrado == nullptr || (cor_led_integrado_enviada && cor == ultima_cor_led_integrado))
        return ESP_OK;

    // Uma transferência pendente conserva o buffer. A tarefa tentará de novo.
    const esp_err_t disponivel = rmt_tx_wait_all_done(canal_led_integrado, 0);
    if (disponivel != ESP_OK) return disponivel;

    const uint8_t bytes[] = {cor.verde, cor.vermelho, cor.azul};
    size_t indice = 0;
    for (const uint8_t byte : bytes) {
        for (uint8_t mascara = 0x80; mascara != 0; mascara >>= 1) {
            const bool bit = (byte & mascara) != 0;
            auto& simbolo = quadro_led_integrado[indice++];
            simbolo.level0 = 1;
            simbolo.duration0 = bit ? 8 : 4;
            simbolo.level1 = 0;
            simbolo.duration1 = bit ? 5 : 9;
        }
    }
    // 300 µs em nível baixo fixa a cor no LED.
    auto& reset = quadro_led_integrado[indice];
    reset.level0 = 0;
    reset.duration0 = 1500;
    reset.level1 = 0;
    reset.duration1 = 1500;
    rmt_transmit_config_t transmissao{};
    transmissao.flags.queue_nonblocking = 1;
    const esp_err_t erro = rmt_transmit(canal_led_integrado, codificador_led_integrado,
        quadro_led_integrado, sizeof(quadro_led_integrado), &transmissao);
    if (erro == ESP_OK) {
        ultima_cor_led_integrado = cor;
        cor_led_integrado_enviada = true;
    }
    return erro;
}

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

esp_err_t configurar_pwm_buzzer(uint32_t frequencia_hz) {
    ledc_timer_config_t temporizador{};
    temporizador.speed_mode = configuracao::MODO_PWM_BUZZER;
    temporizador.duty_resolution = configuracao::RESOLUCAO_PWM_BUZZER;
    temporizador.timer_num = configuracao::TEMPORIZADOR_PWM_BUZZER;
    temporizador.freq_hz = frequencia_hz;
    temporizador.clk_cfg = LEDC_AUTO_CLK;
    return ledc_timer_config(&temporizador);
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
        (1ULL << configuracao::PINO_LED_VERMELHO) |
        (1ULL << configuracao::PINO_LED_AMARELO);
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
    if (erro == ESP_OK) erro = escrever_led(configuracao::PINO_LED_VERMELHO, false);
    if (erro == ESP_OK) erro = escrever_led(configuracao::PINO_LED_AMARELO, false);
    if (erro != ESP_OK) {
        registrar_erro(erro);
        ESP_LOGE(ETIQUETA, "Falha ao apagar os LEDs na inicialização: %s",
                 esp_err_to_name(erro));
        return erro;
    }

    erro = configurar_pwm_buzzer(configuracao::FREQUENCIA_INICIAL_BUZZER_HZ);
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
    const esp_err_t erro_integrado = iniciar_led_integrado();
    if (erro_integrado != ESP_OK)
        ESP_LOGW(ETIQUETA, "LED integrado no GPIO%d indisponível: %s",
                 configuracao::PINO_LED_INTEGRADO, esp_err_to_name(erro_integrado));
    ESP_LOGI(ETIQUETA,
             "Indicadores prontos: LEDs vermelho/verde/amarelo=%d/%d/%d | buzzer passivo GPIO %d",
             configuracao::PINO_LED_VERMELHO, configuracao::PINO_LED_VERDE, configuracao::PINO_LED_AMARELO,
             configuracao::PINO_BUZZER_PASSIVO);
    return ESP_OK;
}

esp_err_t gerenciador_indicadores_definir_leds(const EstadoLeds& estado) {
    if (!estatisticas.iniciado || mutex_indicadores == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(mutex_indicadores, portMAX_DELAY);
    esp_err_t erro = escrever_led(configuracao::PINO_LED_VERDE, estado.verde);
    if (erro == ESP_OK) erro = escrever_led(configuracao::PINO_LED_VERMELHO, estado.vermelho);
    if (erro == ESP_OK) erro = escrever_led(configuracao::PINO_LED_AMARELO, estado.amarelo);
    const esp_err_t erro_integrado = escrever_led_integrado(estado.integrado);
    if (erro == ESP_OK) erro = erro_integrado;
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
    // ledc_set_freq reutiliza a resolução lida do timer. Após sessões de
    // Wi-Fi ela foi lida como zero; reconfigurar restaura resolução e clock.
    esp_err_t erro = configurar_pwm_buzzer(frequencia_hz);
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
