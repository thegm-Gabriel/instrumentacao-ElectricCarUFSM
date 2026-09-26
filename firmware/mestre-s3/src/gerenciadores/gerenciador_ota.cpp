#include "gerenciadores/gerenciador_ota.h"

#include <cstring>

#include "esp_log.h"
#include "nucleo/configuracao_placa.h"

namespace {
constexpr char ETIQUETA[] = "gerenciador_ota";
bool configurado = false;
}

esp_err_t gerenciador_ota_iniciar(const ConfiguracaoOta& configuracao_ota) {
    if (configuracao_ota.endereco_https == nullptr || configuracao_ota.tempo_limite_ms == 0 ||
        std::strncmp(configuracao_ota.endereco_https, "https://", 8) != 0) {
        ESP_LOGE(ETIQUETA, "Configuracao OTA invalida; use uma URL HTTPS");
        return ESP_ERR_INVALID_ARG;
    }
    if (!configuracao::HABILITAR_OTA) {
        ESP_LOGW(ETIQUETA, "OTA preparado, mas desabilitado ate definir servidor e particoes");
        return ESP_ERR_NOT_SUPPORTED;
    }
    configurado = false;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gerenciador_ota_executar() {
    if (!configurado) {
        ESP_LOGE(ETIQUETA, "OTA solicitado antes da configuracao");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

bool gerenciador_ota_esta_configurado() { return configurado; }
