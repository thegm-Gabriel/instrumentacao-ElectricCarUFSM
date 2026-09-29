#include "gerenciadores/gerenciador_ota_satelites.h"

#include <cctype>
#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nucleo/configuracao_placa.h"
#include "protocolo_ota_satelites.h"

namespace {
constexpr char ETIQUETA[] = "ota_uart";
constexpr uint32_t TEMPO_RESPOSTA_MS = 1500;
constexpr uint32_t TEMPO_INICIO_MS = 10000;
constexpr uint32_t TEMPO_FINALIZACAO_MS = 15000;
constexpr unsigned MAXIMO_TENTATIVAS = 4;

static_assert(OTA_SATELITE_TAMANHO_MAXIMO_QUADRO <=
                  configuracao::TAMANHO_MAXIMO_MENSAGEM_UART,
              "O gerenciador UART precisa comportar um quadro OTA completo");

struct RespostaRecebida {
    DestinoUart origem;
    cabecalho_quadro_ota_satelite_t cabecalho;
    carga_resposta_ota_satelite_t carga;
};

struct ContextoDownload {
    ConfiguracaoTransferenciaOtaSatelite configuracao;
    ObservadorProgressoOtaSatelite observador;
    uint32_t sessao;
    uint32_t sequencia;
    uint32_t bytes_confirmados;
    int64_t inicio_ms;
    uint8_t sha256[OTA_SATELITE_TAMANHO_SHA256];
    uint8_t bloco[OTA_SATELITE_TAMANHO_MAXIMO_CARGA];
    size_t ocupacao_bloco;
    bool transferencia_iniciada;
    esp_err_t erro;
};

QueueHandle_t fila_respostas = nullptr;
bool transferencia_em_andamento = false;
portMUX_TYPE trava_estado = portMUX_INITIALIZER_UNLOCKED;

const char* nome_destino(DestinoUart destino) {
    return destino == DestinoUart::Equipe ? "equipe" : "visitantes";
}

bool converter_sha256(const char* texto, uint8_t destino[OTA_SATELITE_TAMANHO_SHA256]) {
    if (texto == nullptr || std::strlen(texto) != 64) return false;
    for (size_t indice = 0; indice < OTA_SATELITE_TAMANHO_SHA256; ++indice) {
        const char alto = texto[indice * 2];
        const char baixo = texto[indice * 2 + 1];
        if (!std::isxdigit(static_cast<unsigned char>(alto)) ||
            !std::isxdigit(static_cast<unsigned char>(baixo))) return false;
        auto valor = [](char caractere) -> uint8_t {
            if (caractere >= '0' && caractere <= '9') return caractere - '0';
            caractere = static_cast<char>(std::tolower(static_cast<unsigned char>(caractere)));
            return static_cast<uint8_t>(caractere - 'a' + 10);
        };
        destino[indice] = static_cast<uint8_t>((valor(alto) << 4) | valor(baixo));
    }
    return true;
}

void limpar_respostas_pendentes() {
    RespostaRecebida descartada{};
    while (xQueueReceive(fila_respostas, &descartada, 0) == pdTRUE) {}
}

esp_err_t enviar_e_confirmar(ContextoDownload& contexto, uint8_t tipo,
                             uint32_t sequencia, uint32_t deslocamento,
                             const void* carga, uint16_t tamanho_carga,
                             uint32_t deslocamento_esperado,
                             fase_ota_satelite_t fase_esperada) {
    uint8_t quadro[OTA_SATELITE_TAMANHO_MAXIMO_QUADRO];
    const size_t tamanho = protocolo_ota_satelites_montar_quadro(
        quadro, sizeof(quadro), tipo, contexto.sessao, sequencia,
        deslocamento, carga, tamanho_carga);
    if (tamanho == 0) return ESP_ERR_INVALID_SIZE;

    for (unsigned tentativa = 1; tentativa <= MAXIMO_TENTATIVAS; ++tentativa) {
        const esp_err_t erro_envio = gerenciador_uart_enviar(
            contexto.configuracao.destino, quadro, tamanho, pdMS_TO_TICKS(250));
        if (erro_envio != ESP_OK) {
            ESP_LOGW(ETIQUETA, "%s: tentativa %u/%u falhou ao enfileirar bloco %lu: %s",
                     nome_destino(contexto.configuracao.destino), tentativa,
                     MAXIMO_TENTATIVAS, static_cast<unsigned long>(sequencia),
                     esp_err_to_name(erro_envio));
            continue;
        }

        const uint32_t tempo_resposta_ms = tipo == OTA_SATELITE_TIPO_INICIAR
            ? TEMPO_INICIO_MS
            : (tipo == OTA_SATELITE_TIPO_FINALIZAR
                   ? TEMPO_FINALIZACAO_MS : TEMPO_RESPOSTA_MS);
        const TickType_t inicio_espera = xTaskGetTickCount();
        const TickType_t tempo_resposta = pdMS_TO_TICKS(tempo_resposta_ms);
        RespostaRecebida resposta{};
        while (true) {
            const TickType_t decorrido = xTaskGetTickCount() - inicio_espera;
            const TickType_t restante = decorrido < tempo_resposta
                                             ? tempo_resposta - decorrido : 0;
            if (xQueueReceive(fila_respostas, &resposta, restante) != pdTRUE) break;
            if (resposta.origem != contexto.configuracao.destino ||
                resposta.cabecalho.sessao != contexto.sessao ||
                resposta.cabecalho.sequencia != sequencia) {
                continue;
            }
            if (resposta.carga.codigo != OTA_SATELITE_RESPOSTA_OK) {
                ESP_LOGE(ETIQUETA,
                         "%s rejeitou o bloco %lu: codigo=%u fase=%u erro=%s proximo=%lu",
                         nome_destino(contexto.configuracao.destino),
                         static_cast<unsigned long>(sequencia),
                         static_cast<unsigned>(resposta.carga.codigo),
                         static_cast<unsigned>(resposta.carga.fase),
                         esp_err_to_name(static_cast<esp_err_t>(resposta.carga.erro_esp)),
                         static_cast<unsigned long>(resposta.cabecalho.deslocamento));
                return resposta.carga.erro_esp == ESP_OK
                           ? ESP_FAIL
                           : static_cast<esp_err_t>(resposta.carga.erro_esp);
            }
            if (resposta.cabecalho.deslocamento != deslocamento_esperado ||
                resposta.carga.fase != fase_esperada) {
                ESP_LOGW(ETIQUETA,
                         "%s respondeu fora da posição esperada: offset=%lu/%lu fase=%u/%u",
                         nome_destino(contexto.configuracao.destino),
                         static_cast<unsigned long>(resposta.cabecalho.deslocamento),
                         static_cast<unsigned long>(deslocamento_esperado),
                         static_cast<unsigned>(resposta.carga.fase),
                         static_cast<unsigned>(fase_esperada));
                continue;
            }
            return ESP_OK;
        }
        ESP_LOGW(ETIQUETA, "%s: bloco %lu sem confirmação; repetindo (%u/%u)",
                 nome_destino(contexto.configuracao.destino),
                 static_cast<unsigned long>(sequencia), tentativa, MAXIMO_TENTATIVAS);
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t iniciar_no_satelite(ContextoDownload& contexto) {
    carga_inicio_ota_satelite_t inicio{};
    inicio.tamanho_firmware = contexto.configuracao.tamanho_bytes;
    std::memcpy(inicio.sha256, contexto.sha256, sizeof(inicio.sha256));
    std::strncpy(inicio.versao, contexto.configuracao.versao,
                 sizeof(inicio.versao) - 1);
    const esp_err_t erro = enviar_e_confirmar(
        contexto, OTA_SATELITE_TIPO_INICIAR, 0, 0, &inicio, sizeof(inicio), 0,
        OTA_SATELITE_FASE_RECEBENDO);
    if (erro == ESP_OK) {
        contexto.transferencia_iniciada = true;
        contexto.inicio_ms = esp_timer_get_time() / 1000;
    }
    return erro;
}

esp_err_t enviar_bloco(ContextoDownload& contexto, const uint8_t* dados,
                       size_t tamanho) {
    if (tamanho == 0 || tamanho > OTA_SATELITE_TAMANHO_MAXIMO_CARGA ||
        contexto.bytes_confirmados + tamanho > contexto.configuracao.tamanho_bytes) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint32_t proximo = contexto.bytes_confirmados + static_cast<uint32_t>(tamanho);
    const esp_err_t erro = enviar_e_confirmar(
        contexto, OTA_SATELITE_TIPO_DADOS, contexto.sequencia,
        contexto.bytes_confirmados, dados, static_cast<uint16_t>(tamanho), proximo,
        OTA_SATELITE_FASE_RECEBENDO);
    if (erro != ESP_OK) return erro;
    contexto.sequencia++;
    contexto.bytes_confirmados = proximo;
    if (contexto.observador != nullptr) {
        const uint32_t decorrido = static_cast<uint32_t>(
            esp_timer_get_time() / 1000 - contexto.inicio_ms);
        contexto.observador(contexto.bytes_confirmados,
                            contexto.configuracao.tamanho_bytes, decorrido);
    }
    return ESP_OK;
}

esp_err_t tratar_evento_http(esp_http_client_event_t* evento) {
    if (evento == nullptr || evento->user_data == nullptr) return ESP_ERR_INVALID_ARG;
    auto& contexto = *static_cast<ContextoDownload*>(evento->user_data);
    if (evento->event_id != HTTP_EVENT_ON_DATA || evento->data == nullptr ||
        evento->data_len <= 0 || contexto.erro != ESP_OK) return contexto.erro;

    if (!contexto.transferencia_iniciada) {
        const int codigo_http = esp_http_client_get_status_code(evento->client);
        // O GitHub pode enviar um pequeno corpo junto com a resposta de
        // redirecionamento. Ele não faz parte do firmware; o cliente HTTP
        // repetirá a requisição usando o endereço indicado no cabeçalho.
        if (codigo_http >= 300 && codigo_http < 400) return ESP_OK;
        if (codigo_http != 200) {
            contexto.erro = ESP_ERR_HTTP_BASE;
            return contexto.erro;
        }
        contexto.erro = iniciar_no_satelite(contexto);
        if (contexto.erro != ESP_OK) return contexto.erro;
    }

    const uint8_t* dados = static_cast<const uint8_t*>(evento->data);
    size_t restantes = static_cast<size_t>(evento->data_len);
    while (restantes > 0 && contexto.erro == ESP_OK) {
        const size_t espaco = sizeof(contexto.bloco) - contexto.ocupacao_bloco;
        const size_t copiar = restantes < espaco ? restantes : espaco;
        std::memcpy(contexto.bloco + contexto.ocupacao_bloco, dados, copiar);
        contexto.ocupacao_bloco += copiar;
        dados += copiar;
        restantes -= copiar;
        if (contexto.ocupacao_bloco == sizeof(contexto.bloco)) {
            contexto.erro = enviar_bloco(contexto, contexto.bloco,
                                          contexto.ocupacao_bloco);
            contexto.ocupacao_bloco = 0;
        }
    }
    return contexto.erro;
}

void cancelar_no_satelite(ContextoDownload& contexto) {
    if (!contexto.transferencia_iniciada) return;
    uint8_t quadro[OTA_SATELITE_TAMANHO_MAXIMO_QUADRO];
    const size_t tamanho = protocolo_ota_satelites_montar_quadro(
        quadro, sizeof(quadro), OTA_SATELITE_TIPO_CANCELAR, contexto.sessao,
        contexto.sequencia, contexto.bytes_confirmados, nullptr, 0);
    if (tamanho != 0) {
        (void)gerenciador_uart_enviar(contexto.configuracao.destino, quadro, tamanho,
                                      pdMS_TO_TICKS(100));
    }
}
}  // namespace

esp_err_t gerenciador_ota_satelites_iniciar() {
    if (fila_respostas != nullptr) return ESP_OK;
    fila_respostas = xQueueCreate(8, sizeof(RespostaRecebida));
    return fila_respostas == nullptr ? ESP_ERR_NO_MEM : ESP_OK;
}

esp_err_t gerenciador_ota_satelites_executar(
    const ConfiguracaoTransferenciaOtaSatelite& configuracao,
    ObservadorProgressoOtaSatelite observador) {
    const bool destino_valido = configuracao.destino == DestinoUart::Equipe ||
                                configuracao.destino == DestinoUart::Visitantes;
    if (fila_respostas == nullptr || configuracao.endereco_https == nullptr ||
        configuracao.versao == nullptr || configuracao.sha256 == nullptr ||
        configuracao.versao[0] == '\0' ||
        std::strlen(configuracao.versao) >= OTA_SATELITE_TAMANHO_VERSAO ||
        configuracao.tamanho_bytes == 0 || configuracao.tempo_limite_http_ms == 0 ||
        !destino_valido ||
        std::strncmp(configuracao.endereco_https, "https://", 8) != 0 ||
        configuracao.endereco_https[8] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&trava_estado);
    if (transferencia_em_andamento) {
        portEXIT_CRITICAL(&trava_estado);
        return ESP_ERR_INVALID_STATE;
    }
    transferencia_em_andamento = true;
    portEXIT_CRITICAL(&trava_estado);

    ContextoDownload contexto{};
    contexto.configuracao = configuracao;
    contexto.observador = observador;
    contexto.sessao = esp_random();
    if (contexto.sessao == 0) contexto.sessao = 1;
    contexto.sequencia = 1;
    contexto.erro = converter_sha256(configuracao.sha256, contexto.sha256)
                         ? ESP_OK : ESP_ERR_INVALID_ARG;
    limpar_respostas_pendentes();

    esp_http_client_config_t http{};
    http.url = configuracao.endereco_https;
    http.event_handler = tratar_evento_http;
    http.user_data = &contexto;
    http.crt_bundle_attach = esp_crt_bundle_attach;
    http.timeout_ms = static_cast<int>(configuracao.tempo_limite_http_ms);
    http.buffer_size = 2048;
    http.buffer_size_tx = 2048;
    http.max_redirection_count = 8;
    http.keep_alive_enable = true;
    http.user_agent = "UFSM-Carro-OTA-Satelites/1";

    esp_http_client_handle_t cliente = contexto.erro == ESP_OK
                                           ? esp_http_client_init(&http) : nullptr;
    if (cliente == nullptr && contexto.erro == ESP_OK) contexto.erro = ESP_ERR_NO_MEM;
    if (contexto.erro == ESP_OK) {
        ESP_LOGW(ETIQUETA, "%s: baixando firmware %s e retransmitindo pela UART",
                 nome_destino(configuracao.destino), configuracao.versao);
        const esp_err_t erro_http = esp_http_client_perform(cliente);
        if (erro_http != ESP_OK && contexto.erro == ESP_OK) contexto.erro = erro_http;
        const int codigo_http = esp_http_client_get_status_code(cliente);
        if (contexto.erro == ESP_OK && codigo_http != 200) contexto.erro = ESP_ERR_HTTP_BASE;
    }
    if (cliente != nullptr) esp_http_client_cleanup(cliente);

    if (contexto.erro == ESP_OK && contexto.ocupacao_bloco > 0) {
        contexto.erro = enviar_bloco(contexto, contexto.bloco,
                                      contexto.ocupacao_bloco);
        contexto.ocupacao_bloco = 0;
    }
    if (contexto.erro == ESP_OK &&
        contexto.bytes_confirmados != configuracao.tamanho_bytes) {
        contexto.erro = ESP_ERR_INVALID_SIZE;
    }
    if (contexto.erro == ESP_OK) {
        contexto.erro = enviar_e_confirmar(
            contexto, OTA_SATELITE_TIPO_FINALIZAR, contexto.sequencia,
            contexto.bytes_confirmados, nullptr, 0, contexto.bytes_confirmados,
            OTA_SATELITE_FASE_PRONTO_PARA_REINICIAR);
    }
    if (contexto.erro != ESP_OK) cancelar_no_satelite(contexto);

    portENTER_CRITICAL(&trava_estado);
    transferencia_em_andamento = false;
    portEXIT_CRITICAL(&trava_estado);
    return contexto.erro;
}

void gerenciador_ota_satelites_registrar_resposta(DestinoUart origem,
                                                  const uint8_t* quadro,
                                                  size_t tamanho) {
    if (fila_respostas == nullptr || quadro == nullptr ||
        !protocolo_ota_satelites_validar_quadro(quadro, tamanho)) return;
    RespostaRecebida resposta{};
    std::memcpy(&resposta.cabecalho, quadro, sizeof(resposta.cabecalho));
    if (resposta.cabecalho.tipo != OTA_SATELITE_TIPO_RESPOSTA ||
        resposta.cabecalho.tamanho_carga != sizeof(resposta.carga)) return;
    resposta.origem = origem;
    std::memcpy(&resposta.carga, quadro + sizeof(resposta.cabecalho),
                sizeof(resposta.carga));
    if (xQueueSend(fila_respostas, &resposta, 0) != pdTRUE) {
        ESP_LOGW(ETIQUETA, "Fila de respostas OTA cheia; confirmação descartada");
    }
}
