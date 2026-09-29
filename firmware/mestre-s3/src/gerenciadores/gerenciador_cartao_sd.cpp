#include "gerenciadores/gerenciador_cartao_sd.h"

#include <cstring>

#include "esp_log.h"

namespace {
constexpr char ETIQUETA[] = "gerenciador_sd";
bool montado = false;
}

esp_err_t gerenciador_cartao_sd_iniciar(const ConfiguracaoCartaoSd& configuracao_sd) {
    if (configuracao_sd.ponto_montagem == nullptr || configuracao_sd.ponto_montagem[0] != '/' ||
        configuracao_sd.pino_cs == GPIO_NUM_NC || configuracao_sd.frequencia_hz == 0) {
        ESP_LOGE(ETIQUETA, "Configuracao do cartao SD invalida");
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGW(ETIQUETA, "Cartao SD ainda não implementado; faltam barramento e pinos");
    montado = false;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gerenciador_cartao_sd_salvar(const char* caminho_relativo, const void* dados,
                                       size_t tamanho, bool acrescentar) {
    (void)acrescentar;
    if (caminho_relativo == nullptr || dados == nullptr || tamanho == 0 ||
        caminho_relativo[0] == '/' || std::strstr(caminho_relativo, "..") != nullptr) {
        ESP_LOGE(ETIQUETA, "Gravacao rejeitada: caminho ou dados invalidos");
        return ESP_ERR_INVALID_ARG;
    }
    if (!montado) {
        ESP_LOGE(ETIQUETA, "Gravacao solicitada sem cartao montado");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gerenciador_cartao_sd_finalizar() {
    if (!montado) {
        ESP_LOGE(ETIQUETA, "Finalizacao solicitada sem cartao montado");
        return ESP_ERR_INVALID_STATE;
    }
    montado = false;
    return ESP_ERR_NOT_SUPPORTED;
}

bool gerenciador_cartao_sd_esta_montado() { return montado; }
