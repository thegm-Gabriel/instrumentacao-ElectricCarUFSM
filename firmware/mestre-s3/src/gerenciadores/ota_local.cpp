// Implementa download HTTPS, escrita e validação da imagem OTA.
#include "gerenciadores/ota_local.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <strings.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nucleo/configuracao/ota.h"

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

esp_err_t verificar_sha256(const esp_partition_t* particao) {
    uint8_t calculado[32]{};
    uint8_t esperado[32]{};
    if (!gerenciador_ota_converter_sha256(sha256_esperado.data(), esperado)) {
        return ESP_ERR_INVALID_ARG;
    }

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

struct RespostaDownload {
    esp_http_client_handle_t cliente = nullptr;
    char intervalo_esperado[64] = {};
    bool intervalo_confirmado = false;
};

esp_err_t registrar_cabecalho(esp_http_client_event_t* evento) {
    auto& resposta = *static_cast<RespostaDownload*>(evento->user_data);
    if (evento->event_id == HTTP_EVENT_ON_CONNECTED) {
        resposta.cliente = evento->client;
        resposta.intervalo_confirmado = false;
    } else if (evento->event_id == HTTP_EVENT_ON_HEADER && evento->header_key != nullptr &&
               evento->header_value != nullptr &&
               strcasecmp(evento->header_key, "Content-Range") == 0) {
        resposta.intervalo_confirmado =
            strcasecmp(evento->header_value, resposta.intervalo_esperado) == 0;
    }
    return ESP_OK;
}

bool falha_transitoria_download(esp_err_t erro) {
    switch (erro) {
        case ESP_FAIL: // O SDK também usa ESP_FAIL para perda da conexão na leitura.
        case ESP_ERR_TIMEOUT:
        case ESP_ERR_HTTP_CONNECT:
        case ESP_ERR_HTTP_FETCH_HEADER:
        case ESP_ERR_HTTP_CONNECTION_CLOSED:
        case ESP_ERR_HTTP_INCOMPLETE_DATA:
        case ESP_ERR_HTTP_EAGAIN:
            return true;
        default:
            return false;
    }
}

esp_err_t baixar_tentativa(
    const esp_partition_t* particao, uint32_t deslocamento,
    RespostaDownload& resposta, esp_https_ota_handle_t& manipulador,
    uint32_t& gravados, bool& repetir, int64_t inicio_us,
    bool (*cancelamento_solicitado)()) {
    repetir = false;
    resposta = {};
    if (deslocamento != 0) {
        std::snprintf(resposta.intervalo_esperado, sizeof(resposta.intervalo_esperado),
                      "bytes %" PRIu32 "-%" PRIu32 "/%" PRIu32,
                      deslocamento, tamanho_esperado - 1, tamanho_esperado);
    }

    esp_http_client_config_t http{};
    http.url = endereco_https.data();
    http.cert_pem = certificado_raiz;
    http.crt_bundle_attach = certificado_raiz == nullptr ? esp_crt_bundle_attach : nullptr;
    http.timeout_ms = static_cast<int>(tempo_limite_ms);
    http.buffer_size = configuracao::TAMANHO_BUFFER_HTTP_RX_MESTRE;
    http.buffer_size_tx = tamanho_buffer_http_tx;
    http.keep_alive_enable = true;
    http.max_redirection_count = configuracao::MAXIMO_REDIRECIONAMENTOS_OTA;
    http.user_agent = "UFSM-Carro-Mestre-OTA/1";
    http.event_handler = registrar_cabecalho;
    http.user_data = &resposta;

    esp_https_ota_config_t ota{};
    ota.http_config = &http;
    if (esp_psram_is_initialized()) ota.buffer_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    ota.partition.staging = particao;
    ota.ota_resumption = deslocamento != 0;
    ota.ota_image_bytes_written = deslocamento;

    esp_err_t erro = esp_https_ota_begin(&ota, &manipulador);
    if (erro != ESP_OK) {
        repetir = falha_transitoria_download(erro);
        return erro;
    }
    if (cancelamento_solicitado()) return ESP_ERR_INVALID_STATE;
    if (resposta.cliente == nullptr ||
        esp_http_client_get_transport_type(resposta.cliente) != HTTP_TRANSPORT_OVER_SSL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const int codigo = esp_http_client_get_status_code(resposta.cliente);
    const int tamanho_http = esp_https_ota_get_image_size(manipulador);
    const uint32_t restante = tamanho_esperado - deslocamento;
    if (deslocamento != 0 &&
        (codigo != 206 || !resposta.intervalo_confirmado ||
         tamanho_http <= 0 || static_cast<uint32_t>(tamanho_http) != restante)) {
        // Reabra desde o início se o servidor não oferecer o intervalo exato.
        ESP_LOGW(ETIQUETA, "Servidor não confirmou o intervalo OTA solicitado (HTTP %d)", codigo);
        gravados = 0;
        repetir = true;
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (deslocamento == 0 && codigo != 200) return ESP_ERR_INVALID_RESPONSE;
    if (tamanho_http > 0 && static_cast<uint32_t>(tamanho_http) != restante) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_app_desc_t nova_imagem{};
    // O descritor já foi recebido e gravado na tentativa inicial.
    erro = deslocamento == 0
        ? esp_https_ota_get_img_desc(manipulador, &nova_imagem)
        : esp_ota_get_partition_description(particao, &nova_imagem);
    if (erro != ESP_OK) {
        repetir = deslocamento == 0 && falha_transitoria_download(erro);
        return erro;
    }
    if (std::strncmp(nova_imagem.version, versao_esperada.data(), sizeof(nova_imagem.version)) != 0 ||
        std::strncmp(nova_imagem.version, esp_app_get_description()->version,
                     sizeof(nova_imagem.version)) == 0) {
        return ESP_ERR_INVALID_VERSION;
    }

    erro = esp_http_client_set_timeout_ms(
        resposta.cliente, configuracao::TEMPO_LEITURA_HTTP_OTA_MS);
    if (erro != ESP_OK) return erro;
    gravados = deslocamento;
    int64_t ultimo_progresso_us = esp_timer_get_time();
    int ultimo_percentual = static_cast<int>(
        (static_cast<uint64_t>(deslocamento) * 100u) / tamanho_esperado) - 10;
    do {
        if (cancelamento_solicitado()) return ESP_ERR_INVALID_STATE;
        erro = esp_https_ota_perform(manipulador);
        const int recebidos = esp_https_ota_get_image_len_read(manipulador);
        if (recebidos > 0 && static_cast<uint32_t>(recebidos) > tamanho_esperado) {
            return ESP_ERR_INVALID_SIZE;
        }
        const int64_t agora_us = esp_timer_get_time();
        if (recebidos >= 0 && static_cast<uint32_t>(recebidos) > gravados) {
            gravados = static_cast<uint32_t>(recebidos);
            ultimo_progresso_us = agora_us;
            if (observador_progresso != nullptr) {
                observador_progresso(gravados, tamanho_esperado,
                    static_cast<uint32_t>((agora_us - inicio_us) / 1000));
            }
            const int percentual = static_cast<int>(
                (static_cast<uint64_t>(gravados) * 100u) / tamanho_esperado);
            if (percentual >= ultimo_percentual + 10 || gravados == tamanho_esperado) {
                ultimo_percentual = percentual;
                ESP_LOGI(ETIQUETA, "Progresso da atualização: %d%% (%" PRIu32 " de %" PRIu32 " bytes)",
                         percentual, gravados, tamanho_esperado);
            }
        }
        if (erro == ESP_ERR_HTTPS_OTA_IN_PROGRESS &&
            agora_us - ultimo_progresso_us >= configuracao::TEMPO_INATIVIDADE_HTTP_OTA_MS * 1000LL) {
            erro = ESP_ERR_TIMEOUT;
        }
    } while (erro == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

    if (erro == ESP_OK && !esp_https_ota_is_complete_data_received(manipulador)) {
        erro = ESP_ERR_HTTP_INCOMPLETE_DATA;
    }
    repetir = falha_transitoria_download(erro);
    if (erro != ESP_OK) return erro;
    return gravados == tamanho_esperado ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t atualizar_particao(const esp_partition_t* particao,
                             bool (*cancelamento_solicitado)(),
                             bool (*autorizar_finalizacao)(),
                             esp_err_t (*restabelecer_rede)()) {
    RespostaDownload resposta{};
    esp_https_ota_handle_t manipulador = nullptr;
    uint32_t gravados = 0;
    const uint32_t setor = esp_partition_get_main_flash_sector_size();
    const int64_t inicio_us = esp_timer_get_time();
    for (unsigned retomadas = 0;; ++retomadas) {
        if (cancelamento_solicitado()) return ESP_ERR_INVALID_STATE;
        if (retomadas != 0) {
            const esp_err_t rede = restabelecer_rede();
            if (cancelamento_solicitado()) return ESP_ERR_INVALID_STATE;
            if (rede != ESP_OK) {
                ESP_LOGW(ETIQUETA, "Não foi possível reconectar para a OTA: %s", esp_err_to_name(rede));
                if (retomadas == configuracao::MAXIMO_RETOMADAS_OTA_MESTRE) return rede;
                vTaskDelay(pdMS_TO_TICKS(configuracao::INTERVALO_RETOMADA_OTA_MS));
                continue;
            }
        }
        // Regrava a partir do começo de um setor para permitir seu apagamento.
        const uint32_t deslocamento = setor == 0 ? 0 :
            (std::min(gravados, tamanho_esperado - 1) / setor) * setor;
        if (deslocamento != 0) ESP_LOGI(ETIQUETA, "Retomando OTA no byte %" PRIu32, deslocamento);
        bool repetir = false;
        const esp_err_t erro = baixar_tentativa(particao, deslocamento, resposta, manipulador,
            gravados, repetir, inicio_us, cancelamento_solicitado);
        if (erro == ESP_OK) break;
        if (manipulador != nullptr) {
            esp_https_ota_abort(manipulador);
            manipulador = nullptr;
        }
        if (cancelamento_solicitado() || !repetir) return erro;
        ESP_LOGW(ETIQUETA, "Transferência interrompida em %" PRIu32 " bytes: %s",
                 gravados, esp_err_to_name(erro));
        if (retomadas == configuracao::MAXIMO_RETOMADAS_OTA_MESTRE) return erro;
        vTaskDelay(pdMS_TO_TICKS(configuracao::INTERVALO_RETOMADA_OTA_MS));
    }

    // Finish valida a imagem e seleciona o próximo boot. Antecipe o SHA do manifesto.
    esp_err_t erro = verificar_sha256(particao);
    if (erro == ESP_OK && !autorizar_finalizacao()) erro = ESP_ERR_INVALID_STATE;
    if (erro != ESP_OK) {
        esp_https_ota_abort(manipulador);
        return erro;
    }
    return esp_https_ota_finish(manipulador);
}
}  // namespace

bool gerenciador_ota_sha256_valido(const char* texto) {
    if (texto == nullptr || std::strlen(texto) != 64) return false;
    for (size_t indice = 0; indice < 64; indice++) {
        if (!std::isxdigit(static_cast<unsigned char>(texto[indice]))) return false;
    }
    return true;
}

bool gerenciador_ota_converter_sha256(const char* texto, uint8_t saida[32]) {
    if (saida == nullptr || !gerenciador_ota_sha256_valido(texto)) return false;
    for (size_t indice = 0; indice < 32; ++indice) {
        saida[indice] = static_cast<uint8_t>(
            (valor_hexadecimal(texto[indice * 2]) << 4) |
            valor_hexadecimal(texto[indice * 2 + 1]));
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

esp_err_t gerenciador_ota_executar(bool (*cancelamento_solicitado)(),
                                   bool (*autorizar_finalizacao)(),
                                   esp_err_t (*restabelecer_rede)()) {
    if (cancelamento_solicitado == nullptr || autorizar_finalizacao == nullptr ||
        restabelecer_rede == nullptr)
        return ESP_ERR_INVALID_ARG;
    if (!configurado || mutex_execucao == nullptr) {
        ESP_LOGE(ETIQUETA, "OTA solicitado antes da configuração");
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(mutex_execucao, 0) != pdTRUE) {
        ESP_LOGW(ETIQUETA, "Já existe uma atualização OTA em andamento");
        return ESP_ERR_INVALID_STATE;
    }
const esp_partition_t* destino = esp_ota_get_next_update_partition(nullptr);
    esp_err_t erro = ESP_ERR_NOT_FOUND;
    if (destino != nullptr) {
        erro = ESP_ERR_INVALID_SIZE;
        if (tamanho_esperado <= destino->size) {
            ESP_LOGI(ETIQUETA, "Iniciando download OTA para a partição '%s'", destino->label);
            erro = atualizar_particao(destino, cancelamento_solicitado,
                                      autorizar_finalizacao, restabelecer_rede);
        }
    }
    xSemaphoreGive(mutex_execucao);
    return erro;
}

esp_err_t gerenciador_ota_confirmar_firmware_em_execucao() {
    const auto* particao = esp_ota_get_running_partition();
    if (particao == nullptr) return ESP_ERR_NOT_FOUND;
    esp_ota_img_states_t estado = ESP_OTA_IMG_UNDEFINED;
    const esp_err_t leitura = esp_ota_get_state_partition(particao, &estado);
    if (leitura != ESP_OK) return leitura;
    if (estado == ESP_OTA_IMG_VALID) return ESP_OK;
    if (estado != ESP_OTA_IMG_PENDING_VERIFY) return ESP_ERR_INVALID_STATE;
    const esp_err_t erro = esp_ota_mark_app_valid_cancel_rollback();
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
