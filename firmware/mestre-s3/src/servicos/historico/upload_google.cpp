#include "servicos/historico/upload_google.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nucleo/tempo.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nucleo/configuracao/historico.h"
#include "servicos/comunicacao/relogio_http.h"
#include "servicos/manutencao/supervisao.h"

namespace {
constexpr char ETIQUETA[] = "registro_telemetria";

esp_err_t preparar_exportacao_google(const char* caminho) {
    FILE* origem = std::fopen(caminho, "rb");
    if (origem == nullptr) return ESP_FAIL;
    FILE* destino = std::fopen(configuracao::ARQUIVO_EXPORTACAO_GOOGLE, "wb");
    if (destino == nullptr) {
        std::fclose(origem);
        return ESP_FAIL;
    }

    // Mantém o cabeçalho e a primeira leitura de cada segundo da sessão.
    char linha[configuracao::TAMANHO_LINHA_CSV_BYTES];
    constexpr char coluna_tempo[] = "tempo_sessao_ms,";
    esp_err_t erro = ESP_OK;
    if (std::fgets(linha, sizeof(linha), origem) == nullptr ||
        std::strncmp(linha, coluna_tempo, sizeof(coluna_tempo) - 1) != 0 ||
        std::strchr(linha, '\n') == nullptr) {
        erro = ESP_ERR_INVALID_RESPONSE;
    } else if (std::fputs(linha, destino) < 0) {
        erro = ESP_FAIL;
    }

    uint32_t ultimo_periodo = UINT32_MAX;
    while (erro == ESP_OK && std::fgets(linha, sizeof(linha), origem) != nullptr) {
        char* fim_tempo = nullptr;
        errno = 0;
        const unsigned long long tempo_ms = std::strtoull(linha, &fim_tempo, 10);
        if (linha[0] < '0' || linha[0] > '9' || *fim_tempo != ',' ||
            errno == ERANGE || tempo_ms > UINT32_MAX || std::strchr(fim_tempo, '\n') == nullptr) {
            erro = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        const auto periodo = static_cast<uint32_t>(tempo_ms) /
            configuracao::INTERVALO_EXPORTACAO_GOOGLE_MS;
        if (periodo == ultimo_periodo) continue;
        if (std::fputs(linha, destino) < 0) {
            erro = ESP_FAIL;
            break;
        }
        ultimo_periodo = periodo;
    }
    if (std::ferror(origem)) erro = ESP_FAIL;
    std::fclose(origem);
    if (std::fclose(destino) != 0) erro = ESP_FAIL;
    if (erro == ESP_OK && ultimo_periodo == UINT32_MAX) erro = ESP_ERR_INVALID_SIZE;
    return erro;
}

struct RespostaHttp {
    char texto[160]{};
    char redirecionamento[2048]{};
    size_t tamanho = 0;
};

esp_err_t tratar_evento_http(esp_http_client_event_t* evento) {
    if (evento == nullptr || evento->user_data == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    auto* resposta = static_cast<RespostaHttp*>(evento->user_data);
    if (evento->event_id == HTTP_EVENT_ON_HEADER) {
        relogio_http_processar_cabecalho(evento->header_key, evento->header_value);
        if (evento->header_key != nullptr && evento->header_value != nullptr &&
            strcasecmp(evento->header_key, "Location") == 0) {
            const size_t tamanho = std::strlen(evento->header_value);
            if (tamanho < sizeof(resposta->redirecionamento)) {
                std::memcpy(resposta->redirecionamento, evento->header_value,
                            tamanho + 1);
            }
        }
        return ESP_OK;
    }
    if (evento->event_id != HTTP_EVENT_ON_DATA || evento->data == nullptr ||
        evento->data_len <= 0) {
        return ESP_OK;
    }
    const size_t disponivel = sizeof(resposta->texto) - 1 - resposta->tamanho;
    const size_t copiar = std::min(
        disponivel, static_cast<size_t>(evento->data_len));
    if (copiar > 0) {
        std::memcpy(resposta->texto + resposta->tamanho, evento->data, copiar);
        resposta->tamanho += copiar;
        resposta->texto[resposta->tamanho] = '\0';
    }
    return ESP_OK;
}

bool caractere_url_seguro(unsigned char caractere) {
    return (caractere >= 'a' && caractere <= 'z') ||
           (caractere >= 'A' && caractere <= 'Z') ||
           (caractere >= '0' && caractere <= '9') || caractere == '-' ||
           caractere == '_' || caractere == '.' || caractere == '~';
}

esp_err_t acrescentar_parametro_url(char* destino, size_t capacidade,
                                    size_t* usados, const char* valor) {
    constexpr char HEXADECIMAL[] = "0123456789ABCDEF";
    for (const unsigned char* atual =
             reinterpret_cast<const unsigned char*>(valor);
         *atual != '\0'; ++atual) {
        if (caractere_url_seguro(*atual)) {
            if (*usados + 1 >= capacidade) return ESP_ERR_INVALID_SIZE;
            destino[(*usados)++] = static_cast<char>(*atual);
        } else {
            if (*usados + 3 >= capacidade) return ESP_ERR_INVALID_SIZE;
            destino[(*usados)++] = '%';
            destino[(*usados)++] = HEXADECIMAL[*atual >> 4];
            destino[(*usados)++] = HEXADECIMAL[*atual & 0x0F];
        }
    }
    destino[*usados] = '\0';
    return ESP_OK;
}

esp_err_t montar_url_upload(const char* nome, char* url, size_t capacidade) {
    const char* base = configuracao::ENDERECO_GOOGLE_TELEMETRIA;
    const size_t tamanho_base = std::strlen(base);
    if (tamanho_base == 0 || tamanho_base + 18 >= capacidade) {
        return ESP_ERR_INVALID_STATE;
    }
    std::memcpy(url, base, tamanho_base);
    size_t usados = tamanho_base;
    url[usados++] = std::strchr(base, '?') == nullptr ? '?' : '&';
    constexpr char PREFIXO_TOKEN[] = "token=";
    std::memcpy(url + usados, PREFIXO_TOKEN, sizeof(PREFIXO_TOKEN) - 1);
    usados += sizeof(PREFIXO_TOKEN) - 1;
    esp_err_t erro = acrescentar_parametro_url(
        url, capacidade, &usados, configuracao::TOKEN_GOOGLE_TELEMETRIA);
    if (erro != ESP_OK) return erro;
    constexpr char PREFIXO_ARQUIVO[] = "&arquivo=";
    if (usados + sizeof(PREFIXO_ARQUIVO) >= capacidade) {
        return ESP_ERR_INVALID_SIZE;
    }
    std::memcpy(url + usados, PREFIXO_ARQUIVO,
                sizeof(PREFIXO_ARQUIVO) - 1);
    usados += sizeof(PREFIXO_ARQUIVO) - 1;
    return acrescentar_parametro_url(url, capacidade, &usados, nome);
}

esp_err_t abrir_arquivo_upload(const char* caminho, FILE** arquivo,
                               size_t* tamanho) {
    if (arquivo == nullptr || tamanho == nullptr) return ESP_ERR_INVALID_ARG;
    *arquivo = std::fopen(caminho, "rb");
    if (*arquivo == nullptr) return ESP_ERR_NOT_FOUND;
    if (std::fseek(*arquivo, 0, SEEK_END) != 0) {
        std::fclose(*arquivo);
        *arquivo = nullptr;
        return ESP_FAIL;
    }
    const long bytes = std::ftell(*arquivo);
    if (bytes <= 0 ||
        static_cast<size_t>(bytes) >
            configuracao::TAMANHO_MAXIMO_CSV_BYTES ||
        std::fseek(*arquivo, 0, SEEK_SET) != 0) {
        std::fclose(*arquivo);
        *arquivo = nullptr;
        return ESP_ERR_INVALID_SIZE;
    }
    *tamanho = static_cast<size_t>(bytes);
    return ESP_OK;
}

esp_err_t transmitir_arquivo(esp_http_client_handle_t cliente, FILE* arquivo,
                             size_t tamanho,
                             const ObservadorUploadGoogle& observador) {
    esp_err_t erro = esp_http_client_open(cliente, static_cast<int>(tamanho));
    if (erro != ESP_OK) return erro;

    char* bloco = static_cast<char*>(heap_caps_malloc(
        configuracao::TAMANHO_BLOCO_UPLOAD_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (bloco == nullptr) {
        bloco = static_cast<char*>(heap_caps_malloc(
            configuracao::TAMANHO_BLOCO_UPLOAD_BYTES, MALLOC_CAP_8BIT));
    }
    if (bloco == nullptr) return ESP_ERR_NO_MEM;

    const uint32_t inicio_ms = tempo_monotonico::agora_ms32();
    uint32_t proximo_relatorio_ms =
        inicio_ms + configuracao::INTERVALO_RELATORIO_UPLOAD_MS;
    size_t enviados = 0;
    while (enviados < tamanho) {
        servico_supervisao_informar_atividade(ModuloSupervisionado::UploadGoogle,
            "upload_csv", "leitura SD/envio HTTPS", 90000);
        const size_t desejados = std::min(
            configuracao::TAMANHO_BLOCO_UPLOAD_BYTES, tamanho - enviados);
        const size_t lidos = std::fread(bloco, 1, desejados, arquivo);
        if (lidos != desejados) {
            erro = ESP_FAIL;
            break;
        }
        size_t escritos_bloco = 0;
        while (escritos_bloco < lidos) {
            const int escritos = esp_http_client_write(
                cliente, bloco + escritos_bloco,
                static_cast<int>(lidos - escritos_bloco));
            if (escritos <= 0) {
                erro = ESP_ERR_HTTP_WRITE_DATA;
                break;
            }
            escritos_bloco += static_cast<size_t>(escritos);
            enviados += static_cast<size_t>(escritos);
        }
        if (erro != ESP_OK) break;

        const uint32_t decorrido_ms = std::max<uint32_t>(
            1, tempo_monotonico::agora_ms32() - inicio_ms);
        const uint32_t velocidade = static_cast<uint32_t>(
            (static_cast<uint64_t>(enviados) * 1000U) / decorrido_ms);
        const uint32_t restantes = enviados >= tamanho
                                       ? 0
                                       : static_cast<uint32_t>(tamanho - enviados);
        const uint32_t tempo_restante = velocidade == 0
            ? 0
            : static_cast<uint32_t>(
                  (static_cast<uint64_t>(restantes) + velocidade - 1U) /
                  velocidade);
        observador.atualizar(static_cast<uint32_t>(enviados), velocidade,
                            tempo_restante);
        const uint32_t instante_ms = tempo_monotonico::agora_ms32();
        if (enviados == tamanho ||
            static_cast<int32_t>(instante_ms - proximo_relatorio_ms) >= 0) {
            const uint32_t percentual_decimos = tamanho == 0
                ? 0
                : static_cast<uint32_t>(
                      (static_cast<uint64_t>(enviados) * 1000U) / tamanho);
            ESP_LOGI(
                ETIQUETA,
                "Upload %lu.%lu%% (%lu/%lu KiB), %lu.%lu KiB/s, restante %lu s",
                static_cast<unsigned long>(percentual_decimos / 10U),
                static_cast<unsigned long>(percentual_decimos % 10U),
                static_cast<unsigned long>(enviados / 1024U),
                static_cast<unsigned long>(tamanho / 1024U),
                static_cast<unsigned long>(
                    velocidade / 1024U),
                static_cast<unsigned long>(
                    ((velocidade % 1024U) * 10U) /
                    1024U),
                static_cast<unsigned long>(
                    tempo_restante));
            proximo_relatorio_ms =
                instante_ms + configuracao::INTERVALO_RELATORIO_UPLOAD_MS;
        }
    }
    heap_caps_free(bloco);
    if (erro != ESP_OK) return erro;

    observador.aguardar_confirmacao(true);
    servico_supervisao_informar_atividade(ModuloSupervisionado::UploadGoogle,
        "upload_csv", "confirmacao Google", 180000);
    const int64_t tamanho_resposta = esp_http_client_fetch_headers(cliente);
    if (tamanho_resposta < 0) {
        return tamanho_resposta == -ESP_ERR_HTTP_EAGAIN
                   ? ESP_ERR_HTTP_EAGAIN
                   : ESP_FAIL;
    }
    int bytes_descartados = 0;
    erro = esp_http_client_flush_response(cliente, &bytes_descartados);
    return erro;
}

esp_err_t enviar_arquivo(const char* caminho, const char* nome, uint32_t indice,
                         uint32_t quantidade, uint32_t* tamanho_enviado,
                         const ObservadorUploadGoogle& observador) {
    servico_supervisao_informar_atividade(ModuloSupervisionado::UploadGoogle,
        "upload_csv", "abertura arquivo/DNS/TLS", 180000);
    FILE* arquivo = nullptr;
    size_t tamanho = 0;
    esp_err_t erro = abrir_arquivo_upload(caminho, &arquivo, &tamanho);
    if (erro != ESP_OK) return erro;
    observador.iniciar(nome, tamanho);
    ESP_LOGI(ETIQUETA, "Enviando arquivo %lu de %lu: %s (%lu KiB)",
             static_cast<unsigned long>(indice),
             static_cast<unsigned long>(quantidade), nome,
             static_cast<unsigned long>(tamanho / 1024U));

    char url[768]{};
    erro = montar_url_upload(nome, url, sizeof(url));
    if (erro != ESP_OK) {
        std::fclose(arquivo);
        return erro;
    }

    RespostaHttp resposta{};
    esp_http_client_config_t http{};
    http.url = url;
    http.method = HTTP_METHOD_POST;
    http.timeout_ms = configuracao::TEMPO_LIMITE_UPLOAD_MS;
    http.buffer_size_tx = configuracao::TAMANHO_BUFFER_TRANSMISSAO_HTTP_BYTES;
    http.event_handler = tratar_evento_http;
    http.user_data = &resposta;
    http.crt_bundle_attach = esp_crt_bundle_attach;
    // O ContentService executa o POST e redireciona a leitura da resposta.
    // O ESP-IDF preserva o método no 302, então a continuação precisa ser manual.
    http.disable_auto_redirect = true;
    esp_http_client_handle_t cliente = esp_http_client_init(&http);
    if (cliente == nullptr) {
        std::fclose(arquivo);
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(cliente, "Content-Type",
                               "text/csv; charset=utf-8");
    erro = transmitir_arquivo(cliente, arquivo, tamanho, observador);
    std::fclose(arquivo);
    const int codigo_post = esp_http_client_get_status_code(cliente);
    esp_http_client_cleanup(cliente);

    char confirmacao[128]{};
    std::snprintf(confirmacao, sizeof(confirmacao), "OK:%s", nome);
    if (erro == ESP_OK && codigo_post == 200 &&
        std::strcmp(resposta.texto, confirmacao) == 0) {
        if (tamanho_enviado != nullptr) {
            *tamanho_enviado = static_cast<uint32_t>(tamanho);
        }
        return ESP_OK;
    }
    const bool redirecionado =
        codigo_post == 301 || codigo_post == 302 || codigo_post == 303 ||
        codigo_post == 307 || codigo_post == 308;
    constexpr char HOST_RESPOSTA[] =
        "https://script.googleusercontent.com/";
    if (erro != ESP_OK || !redirecionado ||
        std::strncmp(resposta.redirecionamento, HOST_RESPOSTA,
                     sizeof(HOST_RESPOSTA) - 1) != 0) {
        ESP_LOGE(ETIQUETA,
                 "POST de %s falhou: transporte=%s HTTP=%d redirecionamento_google=%s resposta='%s'",
                 nome, esp_err_to_name(erro), codigo_post,
                 redirecionado ? "inesperado" : "ausente", resposta.texto);
        return erro != ESP_OK ? erro : ESP_ERR_INVALID_RESPONSE;
    }

    resposta.texto[0] = '\0';
    resposta.tamanho = 0;
    esp_http_client_config_t consulta{};
    consulta.url = resposta.redirecionamento;
    consulta.method = HTTP_METHOD_GET;
    consulta.timeout_ms = configuracao::TEMPO_LIMITE_UPLOAD_MS;
    consulta.buffer_size_tx = configuracao::TAMANHO_BUFFER_TRANSMISSAO_HTTP_BYTES;
    consulta.event_handler = tratar_evento_http;
    consulta.user_data = &resposta;
    consulta.crt_bundle_attach = esp_crt_bundle_attach;
    consulta.max_redirection_count = 2;
    cliente = esp_http_client_init(&consulta);
    if (cliente == nullptr) return ESP_ERR_NO_MEM;
    erro = esp_http_client_perform(cliente);
    const int codigo_http = esp_http_client_get_status_code(cliente);
    esp_http_client_cleanup(cliente);
    if (erro == ESP_OK && codigo_http == 200 &&
        std::strcmp(resposta.texto, confirmacao) == 0) {
        if (tamanho_enviado != nullptr) {
            *tamanho_enviado = static_cast<uint32_t>(tamanho);
        }
        return ESP_OK;
    }
    ESP_LOGE(ETIQUETA,
             "Upload de %s falhou: transporte=%s HTTP=%d resposta='%s'",
             nome, esp_err_to_name(erro), codigo_http, resposta.texto);
    return erro != ESP_OK ? erro : ESP_ERR_INVALID_RESPONSE;
}

}  // namespace

esp_err_t upload_google_enviar(const char* caminho, const char* nome,
                              uint32_t indice, uint32_t quantidade,
                              uint32_t* tamanho_enviado,
                              const ObservadorUploadGoogle& observador) {
    servico_supervisao_informar_atividade(ModuloSupervisionado::UploadGoogle,
        "upload_csv", "preparacao CSV Google", 180000);
    esp_err_t ultimo_erro = preparar_exportacao_google(caminho);
    if (ultimo_erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Falha ao preparar %s para o Google: %s",
                 nome, esp_err_to_name(ultimo_erro));
        (void)std::remove(configuracao::ARQUIVO_EXPORTACAO_GOOGLE);
        return ultimo_erro;
    }
    uint32_t espera_ms = configuracao::ESPERA_INICIAL_NOVA_TENTATIVA_MS;
    for (uint8_t tentativa = 0;
         tentativa < configuracao::MAXIMO_TENTATIVAS_UPLOAD; ++tentativa) {
        if (tentativa > 0) {
            ESP_LOGW(ETIQUETA, "Nova tentativa %u de %u para %s",
                     static_cast<unsigned>(tentativa + 1),
                     static_cast<unsigned>(
                         configuracao::MAXIMO_TENTATIVAS_UPLOAD),
                     nome);
        }
        ultimo_erro = enviar_arquivo(configuracao::ARQUIVO_EXPORTACAO_GOOGLE,
                                     nome, indice, quantidade, tamanho_enviado, observador);
        if (ultimo_erro == ESP_OK) break;
        observador.aguardar_confirmacao(false);
        if (tentativa + 1 < configuracao::MAXIMO_TENTATIVAS_UPLOAD) {
            vTaskDelay(pdMS_TO_TICKS(espera_ms));
            espera_ms = std::min<uint32_t>(espera_ms * 2U, 8000U);
        }
    }
    (void)std::remove(configuracao::ARQUIVO_EXPORTACAO_GOOGLE);
    return ultimo_erro;
}
