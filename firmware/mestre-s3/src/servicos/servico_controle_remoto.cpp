#include "servicos/servico_controle_remoto.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

namespace {
portMUX_TYPE trava = portMUX_INITIALIZER_UNLOCKED;
// Reinício nunca restaura marcha ou liberação de parada de uma sessão anterior.
estado_controle_remoto_t estado{CONTROLE_REMOTO_FORMATO, 0, MARCHA_REMOTA_NEUTRO, 1,
                              0, 0, 0, 0};
}

esp_err_t servico_controle_remoto_definir(AjusteControleRemoto ajuste, uint32_t valor) {
    switch (ajuste) {
        case AjusteControleRemoto::Marcha:
            if (valor > MARCHA_REMOTA_RE) return ESP_ERR_INVALID_ARG;
            break;
        case AjusteControleRemoto::VelocidadeMaxima:
            if (valor > CONTROLE_REMOTO_VELOCIDADE_MAXIMA) return ESP_ERR_INVALID_ARG;
            break;
        case AjusteControleRemoto::AceleracaoMaxima:
            if (valor > CONTROLE_REMOTO_ACELERACAO_MAXIMA) return ESP_ERR_INVALID_ARG;
            break;
        case AjusteControleRemoto::Parada:
            if (valor > 1) return ESP_ERR_INVALID_ARG;
            break;
        default: return ESP_ERR_INVALID_ARG;
    }
    const uint32_t agora = static_cast<uint32_t>(esp_timer_get_time()/1000);
    portENTER_CRITICAL(&trava);
    if (ajuste == AjusteControleRemoto::Marcha && valor != MARCHA_REMOTA_NEUTRO &&
        (estado.parada_solicitada || estado.velocidade_maxima_centesimos_kmh == 0 ||
         estado.aceleracao_maxima_milesimos_ms2 == 0 ||
         (estado.marcha_solicitada != MARCHA_REMOTA_NEUTRO && estado.marcha_solicitada != valor))) {
        portEXIT_CRITICAL(&trava);
        return ESP_ERR_INVALID_STATE;
    }
    if (ajuste == AjusteControleRemoto::Marcha) estado.marcha_solicitada = valor;
    if (ajuste == AjusteControleRemoto::VelocidadeMaxima) estado.velocidade_maxima_centesimos_kmh = valor;
    if (ajuste == AjusteControleRemoto::AceleracaoMaxima) estado.aceleracao_maxima_milesimos_ms2 = valor;
    if (ajuste == AjusteControleRemoto::Parada) estado.parada_solicitada = valor;
    if (estado.parada_solicitada || !estado.velocidade_maxima_centesimos_kmh ||
        !estado.aceleracao_maxima_milesimos_ms2) estado.marcha_solicitada = MARCHA_REMOTA_NEUTRO;
    ++estado.revisao;
    estado.alterado_em_ms = agora;
    const uint32_t revisao = estado.revisao;
    portEXIT_CRITICAL(&trava);
    ESP_LOGI("controle_remoto", "Preparacao sem atuacao: ajuste=%u valor=%lu revisao=%lu",
             static_cast<unsigned>(ajuste), static_cast<unsigned long>(valor),
             static_cast<unsigned long>(revisao));
    return ESP_OK;
}

estado_controle_remoto_t servico_controle_remoto_obter_estado() {
    portENTER_CRITICAL(&trava);
    const auto copia = estado;
    portEXIT_CRITICAL(&trava);
    return copia;
}
