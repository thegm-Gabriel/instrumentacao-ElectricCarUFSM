#include "gerenciadores/gerenciador_i2s.h"

#include "esp_log.h"

namespace {
constexpr char ETIQUETA[] = "gerenciador_i2s";
bool pronto = false;
}

esp_err_t gerenciador_i2s_iniciar(const ConfiguracaoI2s& configuracao_i2s) {
    if (configuracao_i2s.taxa_amostragem_hz == 0 ||
        (configuracao_i2s.bits_por_amostra != 16 && configuracao_i2s.bits_por_amostra != 32) ||
        (configuracao_i2s.quantidade_canais != 1 && configuracao_i2s.quantidade_canais != 2)) {
        ESP_LOGE(ETIQUETA, "Configuracao I2S invalida");
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGW(ETIQUETA, "I2S ainda não implementado; faltam pinos e direção RX/TX");
    pronto = false;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gerenciador_i2s_transmitir(const void* amostras, size_t quantidade_bytes,
                                     TickType_t tempo_limite) {
    (void)tempo_limite;
    if (amostras == nullptr || quantidade_bytes == 0) return ESP_ERR_INVALID_ARG;
    if (!pronto) {
        ESP_LOGE(ETIQUETA, "Transmissao solicitada antes da inicializacao I2S");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

bool gerenciador_i2s_esta_pronto() { return pronto; }
