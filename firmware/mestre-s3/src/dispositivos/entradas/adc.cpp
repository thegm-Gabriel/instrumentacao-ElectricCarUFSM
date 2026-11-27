// Mede o pedal e disponibiliza a última amostra bruta ao diagnóstico.
#include "dispositivos/entradas/adc.h"

#include <atomic>

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "nucleo/configuracao/placa.h"

namespace {
constexpr char ETIQUETA[] = "adc_pedal";
adc_oneshot_unit_handle_t unidade_adc = nullptr;
adc_channel_t canal_pedal{};
adc_cali_handle_t calibracao_pedal = nullptr;
bool inicializado = false;
std::atomic<int> ultimo_bruto{-1};
}

esp_err_t leitor_pedal_iniciar() {
    if (inicializado) return ESP_OK;

    adc_unit_t unidade{};
    esp_err_t erro = adc_oneshot_io_to_channel(configuracao::PINO_ACELERADOR_ADC,
                                               &unidade, &canal_pedal);
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "GPIO %d nao pode ser usado pelo ADC: %s",
                 configuracao::PINO_ACELERADOR_ADC, esp_err_to_name(erro));
        return erro;
    }

    adc_oneshot_unit_init_cfg_t configuracao_unidade{};
    configuracao_unidade.unit_id = unidade;
    configuracao_unidade.ulp_mode = ADC_ULP_MODE_DISABLE;
    erro = adc_oneshot_new_unit(&configuracao_unidade, &unidade_adc);
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Falha ao criar unidade ADC: %s", esp_err_to_name(erro));
        return erro;
    }

    adc_oneshot_chan_cfg_t configuracao_canal{};
    configuracao_canal.atten = ADC_ATTEN_DB_12;
    configuracao_canal.bitwidth = ADC_BITWIDTH_DEFAULT;
    erro = adc_oneshot_config_channel(unidade_adc, canal_pedal, &configuracao_canal);
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Falha ao configurar canal ADC: %s", esp_err_to_name(erro));
        adc_oneshot_del_unit(unidade_adc);
        unidade_adc = nullptr;
        return erro;
    }

    adc_cali_curve_fitting_config_t calibracao{};
    calibracao.unit_id = unidade;
    calibracao.chan = canal_pedal;
    calibracao.atten = ADC_ATTEN_DB_12;
    calibracao.bitwidth = ADC_BITWIDTH_DEFAULT;
    erro = adc_cali_create_scheme_curve_fitting(&calibracao, &calibracao_pedal);
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Falha ao calibrar o ADC do pedal: %s", esp_err_to_name(erro));
        adc_oneshot_del_unit(unidade_adc);
        unidade_adc = nullptr;
        return erro;
    }

    inicializado = true;
    ESP_LOGI(ETIQUETA, "ADC pronto no GPIO %d", configuracao::PINO_ACELERADOR_ADC);
    return ESP_OK;
}

esp_err_t leitor_pedal_ler_mv(uint16_t* milivolts) {
    if (milivolts == nullptr) return ESP_ERR_INVALID_ARG;
    if (!inicializado || calibracao_pedal == nullptr)
        return ESP_ERR_INVALID_STATE;
    int bruto = 0;
    esp_err_t erro = adc_oneshot_read(unidade_adc, canal_pedal, &bruto);
    ultimo_bruto.store(erro == ESP_OK && bruto >= 0 && bruto <= 4095 ? bruto : -1,
                       std::memory_order_relaxed);
    int convertido_mv = 0;
    if (erro == ESP_OK)
        erro = adc_cali_raw_to_voltage(calibracao_pedal, bruto,
                                       &convertido_mv);
    if (erro != ESP_OK || convertido_mv < 0 || convertido_mv > 3300)
        return erro == ESP_OK ? ESP_ERR_INVALID_RESPONSE : erro;
    *milivolts = static_cast<uint16_t>(convertido_mv);
    return ESP_OK;
}

esp_err_t leitor_pedal_obter_bruto(uint16_t* valor_bruto) {
    if (valor_bruto == nullptr) return ESP_ERR_INVALID_ARG;
    const int valor = ultimo_bruto.load(std::memory_order_relaxed);
    if (valor < 0) return ESP_ERR_INVALID_STATE;
    *valor_bruto = static_cast<uint16_t>(valor);
    return ESP_OK;
}
