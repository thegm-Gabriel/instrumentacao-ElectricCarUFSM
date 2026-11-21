// Implementa a aquisição do ADC usado para medir tensão.
#include "dispositivos/entradas/adc.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "nucleo/configuracao/placa.h"

namespace {
constexpr char ETIQUETA[] = "leitor_tensao";
adc_oneshot_unit_handle_t unidade_adc = nullptr;
adc_channel_t canal_adc{};
adc_channel_t canal_pedal{};
adc_cali_handle_t calibracao_pedal = nullptr;
bool inicializado = false;
}

esp_err_t leitor_tensao_iniciar() {
    if (inicializado) return ESP_OK;

    adc_unit_t unidade{};
    esp_err_t erro = adc_oneshot_io_to_channel(configuracao::PINO_LEITURA_TENSAO,
                                               &unidade, &canal_adc);
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "GPIO %d nao pode ser usado pelo ADC: %s",
                 configuracao::PINO_LEITURA_TENSAO, esp_err_to_name(erro));
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
    erro = adc_oneshot_config_channel(unidade_adc, canal_adc, &configuracao_canal);
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Falha ao configurar canal ADC: %s", esp_err_to_name(erro));
        adc_oneshot_del_unit(unidade_adc);
        unidade_adc = nullptr;
        return erro;
    }

    adc_unit_t unidade_pedal{};
    erro = adc_oneshot_io_to_channel(configuracao::PINO_ACELERADOR_ADC,
                                    &unidade_pedal, &canal_pedal);
    if (erro == ESP_OK && unidade_pedal != unidade)
        erro = ESP_ERR_INVALID_ARG;
    if (erro == ESP_OK)
        erro = adc_oneshot_config_channel(unidade_adc, canal_pedal,
                                         &configuracao_canal);
    if (erro == ESP_OK) {
        adc_cali_curve_fitting_config_t configuracao_calibracao{};
        configuracao_calibracao.unit_id = unidade;
        configuracao_calibracao.chan = canal_pedal;
        configuracao_calibracao.atten = ADC_ATTEN_DB_12;
        configuracao_calibracao.bitwidth = ADC_BITWIDTH_DEFAULT;
        erro = adc_cali_create_scheme_curve_fitting(&configuracao_calibracao,
                                                     &calibracao_pedal);
    }
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Pedal ADC indisponível: %s", esp_err_to_name(erro));
        calibracao_pedal = nullptr;
        // O canal bruto já foi configurado e continua disponível à telemetria.
    }

    inicializado = true;
    ESP_LOGI(ETIQUETA, "ADC pronto no GPIO %d", configuracao::PINO_LEITURA_TENSAO);
    return ESP_OK;
}

esp_err_t leitor_pedal_ler_mv(uint16_t* milivolts) {
    if (milivolts == nullptr) return ESP_ERR_INVALID_ARG;
    if (!inicializado || calibracao_pedal == nullptr)
        return ESP_ERR_INVALID_STATE;
    int bruto = 0;
    esp_err_t erro = adc_oneshot_read(unidade_adc, canal_pedal, &bruto);
    int convertido_mv = 0;
    if (erro == ESP_OK)
        erro = adc_cali_raw_to_voltage(calibracao_pedal, bruto,
                                       &convertido_mv);
    if (erro != ESP_OK || convertido_mv < 0 || convertido_mv > 3300)
        return erro == ESP_OK ? ESP_ERR_INVALID_RESPONSE : erro;
    *milivolts = static_cast<uint16_t>(convertido_mv);
    return ESP_OK;
}

esp_err_t leitor_tensao_ler(uint16_t* valor_bruto) {
    if (valor_bruto == nullptr) return ESP_ERR_INVALID_ARG;
    if (!inicializado || unidade_adc == nullptr) return ESP_ERR_INVALID_STATE;

    int valor = 0;
    esp_err_t erro = adc_oneshot_read(unidade_adc, canal_adc, &valor);
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Falha na leitura ADC: %s", esp_err_to_name(erro));
        return erro;
    }
    if (valor < 0 || valor > 4095) {
        ESP_LOGE(ETIQUETA, "ADC retornou valor fora da faixa: %d", valor);
        return ESP_FAIL;
    }
    *valor_bruto = static_cast<uint16_t>(valor);
    return ESP_OK;
}
