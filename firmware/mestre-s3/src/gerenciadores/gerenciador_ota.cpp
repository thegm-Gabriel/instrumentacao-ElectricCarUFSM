#include "gerenciadores/gerenciador_ota.h"

#include <array>
#include <cctype>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace {
constexpr char ETIQUETA[] = "gerenciador_ota";
constexpr size_t TAMANHO_URL = 512;
constexpr size_t TAMANHO_SHA256_TEXTO = 65;

std::array<char, TAMANHO_URL> endereco_https{};
std::array<char, 32> versao_esperada{};
std::array<char, TAMANHO_SHA256_TEXTO> sha256_esperado{};
const char* certificado_raiz = nullptr;
uint32_t tempo_limite_ms = 0;
uint32_t tamanho_esperado = 0;
int tamanho_buffer_http_tx = 0;
ObservadorProgressoOta observador_progresso = nullptr;
bool configurado = false;
SemaphoreHandle_t mutex_execucao = nullptr;

uint8_t valor_hexadecimal(char caractere) {
    if (caractere >= '0' && caractere <= '9') return caractere - '0';
    caractere = static_cast<char>(std::tolower(static_cast<unsigned char>(caractere)));
    return static_cast<uint8_t>(caractere - 'a' + 10);
}

void converter_sha256(const char* texto, uint8_t* saida) {
    for (size_t indice = 0; indice < 32; indice++) {
        saida[indice] = static_cast<uint8_t>((valor_hexadecimal(texto[indice * 2]) << 4) |
                                              valor_hexadecimal(texto[indice * 2 + 1]));
    }
}

esp_err_t verificar_sha256(const esp_partition_t* particao) {
    uint8_t calculado[32]{};
    uint8_t esperado[32]{};
    converter_sha256(sha256_esperado.data(), esperado);

    esp_err_t erro = esp_partition_get_sha256(particao, calculado);
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Não foi possível calcular SHA-256: %s", esp_err_to_name(erro));
        return erro;
    }
    if (std::memcmp(calculado, esperado, sizeof(calculado)) != 0) {
        ESP_LOGE(ETIQUETA, "SHA-256 da imagem difere do manifesto");
        return ESP_ERR_INVALID_CRC;
    }
    ESP_LOGI(ETIQUETA,
             "Integridade confirmada: o SHA-256 recebido corresponde ao manifesto");
    return ESP_OK;
}

bool obter_estado_em_execucao(esp_ota_img_states_t* estado) {
    const esp_partition_t* particao = esp_ota_get_running_partition();
    return particao != nullptr && esp_ota_get_state_partition(particao, estado) == ESP_OK;
}
}  // namespace

bool gerenciador_ota_sha256_valido(const char* texto) {
    if (texto == nullptr || std::strlen(texto) != 64) return false;
    for (size_t indice = 0; indice < 64; indice++) {
        if (!std::isxdigit(static_cast<unsigned char>(texto[indice]))) return false;
    }
    return true;
}

esp_err_t gerenciador_ota_iniciar(const ConfiguracaoOta& configuracao_ota) {
    if (configuracao_ota.endereco_https == nullptr ||
        std::strncmp(configuracao_ota.endereco_https, "https://", 8) != 0 ||
        configuracao_ota.endereco_https[8] == '\0' ||
        std::strlen(configuracao_ota.endereco_https) >= endereco_https.size() ||
        configuracao_ota.versao_esperada == nullptr ||
        configuracao_ota.versao_esperada[0] == '\0' ||
        std::strlen(configuracao_ota.versao_esperada) >= versao_esperada.size() ||
        !gerenciador_ota_sha256_valido(configuracao_ota.sha256_esperado) ||
        configuracao_ota.tempo_limite_ms == 0 ||
        configuracao_ota.tamanho_esperado == 0 ||
        configuracao_ota.tamanho_buffer_http_tx <= 0) {
        ESP_LOGE(ETIQUETA, "Configuração OTA inválida");
        return ESP_ERR_INVALID_ARG;
    }
    if (esp_ota_get_next_update_partition(nullptr) == nullptr) {
        ESP_LOGE(ETIQUETA, "Tabela de partições não possui um destino OTA");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (mutex_execucao == nullptr) mutex_execucao = xSemaphoreCreateMutex();
    if (mutex_execucao == nullptr) return ESP_ERR_NO_MEM;
    if (xSemaphoreTake(mutex_execucao, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;

    std::strcpy(endereco_https.data(), configuracao_ota.endereco_https);
    std::strcpy(versao_esperada.data(), configuracao_ota.versao_esperada);
    std::strcpy(sha256_esperado.data(), configuracao_ota.sha256_esperado);
    certificado_raiz = configuracao_ota.certificado_raiz;
    tempo_limite_ms = configuracao_ota.tempo_limite_ms;
    tamanho_esperado = configuracao_ota.tamanho_esperado;
    tamanho_buffer_http_tx = configuracao_ota.tamanho_buffer_http_tx;
    observador_progresso = configuracao_ota.observador_progresso;
    configurado = true;
    xSemaphoreGive(mutex_execucao);

    ESP_LOGI(ETIQUETA, "Download OTA preparado por HTTPS");
    return ESP_OK;
}

esp_err_t gerenciador_ota_executar() {
    if (!configurado || mutex_execucao == nullptr) {
        ESP_LOGE(ETIQUETA, "OTA solicitado antes da configuração");
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(mutex_execucao, 0) != pdTRUE) {
        ESP_LOGW(ETIQUETA, "Já existe uma atualização OTA em andamento");
        return ESP_ERR_INVALID_STATE;
    }

    const esp_partition_t* particao_atual = esp_ota_get_running_partition();
    const esp_partition_t* particao_destino = esp_ota_get_next_update_partition(nullptr);
    if (particao_atual == nullptr || particao_destino == nullptr) {
        xSemaphoreGive(mutex_execucao);
        return ESP_ERR_NOT_FOUND;
    }

    esp_http_client_config_t http{};
    http.url = endereco_https.data();
    http.cert_pem = certificado_raiz;
    http.crt_bundle_attach = certificado_raiz == nullptr ? esp_crt_bundle_attach : nullptr;
    http.timeout_ms = static_cast<int>(tempo_limite_ms);
    http.buffer_size_tx = tamanho_buffer_http_tx;
    http.keep_alive_enable = true;
    http.max_redirection_count = 8;
    http.user_agent = "UFSM-Carro-Mestre-OTA/1";

    esp_https_ota_config_t ota{};
    ota.http_config = &http;

    ESP_LOGW(ETIQUETA, "Iniciando download e gravação na partição segura '%s'",
             particao_destino->label);
    esp_https_ota_handle_t manipulador = nullptr;
    esp_err_t erro = esp_https_ota_begin(&ota, &manipulador);
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Falha ao iniciar download: %s", esp_err_to_name(erro));
        xSemaphoreGive(mutex_execucao);
        return erro;
    }

    esp_app_desc_t nova_imagem{};
    erro = esp_https_ota_get_img_desc(manipulador, &nova_imagem);
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Cabeçalho da nova imagem inválido: %s", esp_err_to_name(erro));
        esp_https_ota_abort(manipulador);
        xSemaphoreGive(mutex_execucao);
        return erro;
    }

    const esp_app_desc_t* imagem_atual = esp_app_get_description();
    ESP_LOGI(ETIQUETA, "Versão instalada: %s | versão recebida: %s",
             imagem_atual->version, nova_imagem.version);
    if (std::strcmp(nova_imagem.version, versao_esperada.data()) != 0) {
        ESP_LOGE(ETIQUETA, "Versão da imagem (%s) difere do manifesto (%s)",
                 nova_imagem.version, versao_esperada.data());
        esp_https_ota_abort(manipulador);
        xSemaphoreGive(mutex_execucao);
        return ESP_ERR_INVALID_VERSION;
    }
    if (std::strcmp(imagem_atual->version, nova_imagem.version) == 0) {
        ESP_LOGW(ETIQUETA, "A imagem recebida possui a mesma versão em execução");
        esp_https_ota_abort(manipulador);
        xSemaphoreGive(mutex_execucao);
        return ESP_ERR_INVALID_VERSION;
    }

    const int tamanho_http = esp_https_ota_get_image_size(manipulador);
    if (tamanho_http > 0 && static_cast<uint32_t>(tamanho_http) != tamanho_esperado) {
        ESP_LOGE(ETIQUETA, "Servidor informou %d bytes; manifesto informa %lu bytes",
                 tamanho_http, static_cast<unsigned long>(tamanho_esperado));
        esp_https_ota_abort(manipulador);
        xSemaphoreGive(mutex_execucao);
        return ESP_ERR_INVALID_SIZE;
    }
    const uint32_t tamanho_total = tamanho_esperado;
    if (tamanho_total > particao_destino->size) {
        ESP_LOGE(ETIQUETA, "Imagem de %lu bytes excede a partição de %lu bytes",
                 static_cast<unsigned long>(tamanho_total),
                 static_cast<unsigned long>(particao_destino->size));
        esp_https_ota_abort(manipulador);
        xSemaphoreGive(mutex_execucao);
        return ESP_ERR_INVALID_SIZE;
    }

    int ultimo_percentual = -10;
    const int64_t inicio_ms = esp_timer_get_time() / 1000;
    do {
        erro = esp_https_ota_perform(manipulador);
        const int recebidos = esp_https_ota_get_image_len_read(manipulador);
        if (recebidos >= 0) {
            if (observador_progresso != nullptr) {
                observador_progresso(
                    static_cast<uint32_t>(recebidos),
                    tamanho_total,
                    static_cast<uint32_t>(esp_timer_get_time() / 1000 - inicio_ms));
            }
            const int percentual = static_cast<int>(
                (static_cast<uint64_t>(recebidos) * 100u) / tamanho_total);
            if (percentual >= ultimo_percentual + 10) {
                ultimo_percentual = percentual;
                ESP_LOGI(ETIQUETA, "Progresso da atualização: %d%% (%d de %d bytes)",
                         percentual, recebidos, static_cast<int>(tamanho_total));
            }
        }
    } while (erro == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

    if (erro != ESP_OK || !esp_https_ota_is_complete_data_received(manipulador)) {
        if (erro == ESP_OK) erro = ESP_ERR_HTTP_INCOMPLETE_DATA;
        ESP_LOGE(ETIQUETA, "Download OTA incompleto: %s", esp_err_to_name(erro));
        esp_https_ota_abort(manipulador);
        xSemaphoreGive(mutex_execucao);
        return erro;
    }
    if (static_cast<uint32_t>(esp_https_ota_get_image_len_read(manipulador)) !=
        tamanho_total) {
        ESP_LOGE(ETIQUETA, "Imagem recebida com tamanho diferente do manifesto");
        esp_https_ota_abort(manipulador);
        xSemaphoreGive(mutex_execucao);
        return ESP_ERR_INVALID_SIZE;
    }

    erro = esp_https_ota_finish(manipulador);
    if (erro == ESP_OK) erro = verificar_sha256(particao_destino);
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Imagem OTA rejeitada: %s", esp_err_to_name(erro));
        esp_err_t erro_restaura = esp_ota_set_boot_partition(particao_atual);
        if (erro_restaura != ESP_OK) {
            ESP_LOGE(ETIQUETA, "Falha crítica ao restaurar partição de boot: %s",
                     esp_err_to_name(erro_restaura));
        }
        xSemaphoreGive(mutex_execucao);
        return erro;
    }

    ESP_LOGI(ETIQUETA,
             "Nova imagem gravada e validada. Ela será usada na próxima inicialização");
    xSemaphoreGive(mutex_execucao);
    return ESP_OK;
}

esp_err_t gerenciador_ota_confirmar_firmware_em_execucao() {
    esp_ota_img_states_t estado;
    if (!obter_estado_em_execucao(&estado) || estado != ESP_OTA_IMG_PENDING_VERIFY) return ESP_OK;
    esp_err_t erro = esp_ota_mark_app_valid_cancel_rollback();
    if (erro == ESP_OK) {
        ESP_LOGI(ETIQUETA,
                 "Autoteste concluído. O novo firmware foi confirmado como válido");
    } else {
        ESP_LOGE(ETIQUETA, "Falha ao confirmar firmware: %s", esp_err_to_name(erro));
    }
    return erro;
}

esp_err_t gerenciador_ota_rejeitar_firmware_em_execucao() {
    esp_ota_img_states_t estado;
    if (!obter_estado_em_execucao(&estado) || estado != ESP_OTA_IMG_PENDING_VERIFY) return ESP_OK;
    ESP_LOGE(ETIQUETA, "Autoteste falhou; retornando ao firmware anterior");
    return esp_ota_mark_app_invalid_rollback_and_reboot();
}
