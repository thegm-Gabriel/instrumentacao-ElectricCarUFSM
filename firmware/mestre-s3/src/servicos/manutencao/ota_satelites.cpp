// Encadeia download HTTPS e envio UART usando buffers fixos reutilizáveis.
#include "nucleo/configuracao/tarefas.h"
#include "servicos/manutencao/supervisao.h"
#include "servicos/manutencao/ota_satelites.h"

#include <algorithm>
#include <cstring>

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gerenciadores/ota_local.h"
#include "gerenciadores/uart.h"
#include "nucleo/configuracao/placa.h"
#include "nucleo/configuracao/comunicacao.h"
#include "nucleo/configuracao/ota.h"
#include "protocolo_ota_satelites.h"

namespace {
constexpr char ETIQUETA[] = "ota_uart";
constexpr uint8_t INDICE_BUFFER_INVALIDO = UINT8_MAX;
constexpr TickType_t INTERVALO_ESPERA_PIPELINE = pdMS_TO_TICKS(100);
constexpr TickType_t TEMPO_LIMITE_PIPELINE = pdMS_TO_TICKS(30000);
constexpr size_t TAMANHO_BLOCOS_PIPELINE =
    static_cast<size_t>(configuracao::QUANTIDADE_BUFFERS_PIPELINE) * OTA_SATELITE_TAMANHO_MAXIMO_CARGA;
static_assert(configuracao::TEMPO_INATIVIDADE_HTTP_OTA_MS < OTA_SATELITE_INATIVIDADE_MS);

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
    bool (*cancelamento_solicitado)();
    uint32_t sessao;
    uint32_t sequencia;
    uint32_t bytes_confirmados;
    int64_t inicio_ms;
    uint8_t sha256[OTA_SATELITE_TAMANHO_SHA256];
    uint8_t* quadro = nullptr;
    uint8_t indice_bloco_atual = INDICE_BUFFER_INVALIDO;
    size_t ocupacao_bloco;
    TaskHandle_t tarefa_solicitante = nullptr;
    bool transferencia_iniciada;
    esp_err_t erro;
};

struct BlocoPipeline {
    uint8_t indice = INDICE_BUFFER_INVALIDO;
    uint16_t tamanho = 0;
    bool finalizar = false;
    bool encerrar_tarefa = false;
};

QueueHandle_t fila_respostas = nullptr;
QueueHandle_t fila_buffers_livres = nullptr;
QueueHandle_t fila_blocos_prontos = nullptr;
TaskHandle_t tarefa_pipeline_handle = nullptr;
uint8_t* memoria_pipeline = nullptr;
bool pipeline_na_psram = false;
ContextoDownload contexto_download{};
ContextoDownload* contexto_pipeline = nullptr;
bool transferencia_em_andamento = false;
bool pipeline_drenado = false;
bool produtor_abandonou_pipeline = false;
bool pipeline_operacional = false;
portMUX_TYPE trava_estado = portMUX_INITIALIZER_UNLOCKED;

const char* nome_destino(DestinoUart destino) {
    return destino == DestinoUart::Equipe ? "equipe" : "visitantes";
}

uint8_t* obter_buffer(uint8_t indice) {
    if (memoria_pipeline == nullptr || indice >= configuracao::QUANTIDADE_BUFFERS_PIPELINE) {
        return nullptr;
    }
    return memoria_pipeline +
           static_cast<size_t>(indice) * OTA_SATELITE_TAMANHO_MAXIMO_CARGA;
}

esp_err_t obter_erro(const ContextoDownload& contexto) {
    portENTER_CRITICAL(&trava_estado);
    const esp_err_t erro = contexto.erro;
    portEXIT_CRITICAL(&trava_estado);
    return erro;
}

void definir_primeiro_erro(ContextoDownload& contexto, esp_err_t erro) {
    if (erro == ESP_OK) return;
    portENTER_CRITICAL(&trava_estado);
    if (contexto.erro == ESP_OK) contexto.erro = erro;
    portEXIT_CRITICAL(&trava_estado);
}

bool cancelamento_ativo(const ContextoDownload& contexto) {
    return contexto.cancelamento_solicitado();
}

void cancelar_no_satelite(ContextoDownload& contexto);

void liberar_sessao() {
    portENTER_CRITICAL(&trava_estado);
    contexto_pipeline = nullptr;
    transferencia_em_andamento = false;
    portEXIT_CRITICAL(&trava_estado);
}

void limpar_respostas_pendentes() {
    RespostaRecebida descartada{};
    while (xQueueReceive(fila_respostas, &descartada, 0) == pdTRUE) {}
}

esp_err_t enviar_e_confirmar(ContextoDownload& contexto, uint8_t tipo,
                             uint32_t sequencia, uint32_t deslocamento,
                             const void* carga, uint16_t tamanho_carga,
                             uint32_t deslocamento_esperado,
                             fase_ota_satelite_t fase_esperada,
                             bool* finalizacao_incerta = nullptr) {
    const size_t tamanho = protocolo_ota_satelites_montar_quadro(
        contexto.quadro, OTA_SATELITE_TAMANHO_MAXIMO_QUADRO, tipo, contexto.sessao, sequencia,
        deslocamento, carga, tamanho_carga);
    if (tamanho == 0) return ESP_ERR_INVALID_SIZE;

    const unsigned tentativas = finalizacao_incerta == nullptr
        ? configuracao::MAXIMO_TENTATIVAS_OTA_UART
        : configuracao::MAXIMO_TENTATIVAS_FINALIZACAO_OTA_UART;
    for (unsigned tentativa = 1; tentativa <= tentativas; ++tentativa) {
        if (cancelamento_ativo(contexto)) return ESP_ERR_INVALID_STATE;
        const esp_err_t erro_pipeline = obter_erro(contexto);
        if (erro_pipeline != ESP_OK) return erro_pipeline;
        if (finalizacao_incerta != nullptr) *finalizacao_incerta = true;
        const esp_err_t erro_envio = gerenciador_uart_enviar(
            contexto.configuracao.destino, contexto.quadro, tamanho,
            pdMS_TO_TICKS(250));
        if (erro_envio != ESP_OK) {
            ESP_LOGW(ETIQUETA, "%s: tentativa %u/%u falhou ao transmitir bloco %lu: %s",
                     nome_destino(contexto.configuracao.destino), tentativa,
                     tentativas, static_cast<unsigned long>(sequencia),
                     esp_err_to_name(erro_envio));
            continue;
        }

        const uint32_t tempo_resposta_ms = tipo == OTA_SATELITE_TIPO_INICIAR
            ? configuracao::TEMPO_INICIO_OTA_UART_MS
            : (tipo == OTA_SATELITE_TIPO_FINALIZAR
                   ? configuracao::TEMPO_FINALIZACAO_OTA_UART_MS : configuracao::TEMPO_RESPOSTA_OTA_UART_MS);
        const TickType_t inicio_espera = xTaskGetTickCount();
        const TickType_t tempo_resposta = pdMS_TO_TICKS(tempo_resposta_ms);
        RespostaRecebida resposta{};
        while (true) {
            if (cancelamento_ativo(contexto)) return ESP_ERR_INVALID_STATE;
            const esp_err_t falha = obter_erro(contexto);
            if (falha != ESP_OK) return falha;
            const TickType_t decorrido = xTaskGetTickCount() - inicio_espera;
            const TickType_t restante = decorrido < tempo_resposta
                                             ? tempo_resposta - decorrido : 0;
            if (restante == 0) break;
            const TickType_t espera =
                std::min(restante, INTERVALO_ESPERA_PIPELINE);
            if (xQueueReceive(fila_respostas, &resposta, espera) != pdTRUE) {
                continue;
            }
            if (resposta.origem != contexto.configuracao.destino ||
                resposta.cabecalho.sessao != contexto.sessao ||
                resposta.cabecalho.sequencia != sequencia) {
                continue;
            }
            if (resposta.carga.codigo != OTA_SATELITE_RESPOSTA_OK) {
                if (finalizacao_incerta != nullptr && tentativa == 1) *finalizacao_incerta = false;
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
            if (finalizacao_incerta != nullptr) *finalizacao_incerta = false;
            return ESP_OK;
        }
        ESP_LOGW(ETIQUETA, "%s: bloco %lu sem confirmação; repetindo (%u/%u)",
                 nome_destino(contexto.configuracao.destino),
                 static_cast<unsigned long>(sequencia), tentativa, tentativas);
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t iniciar_no_satelite(ContextoDownload& contexto) {
    carga_inicio_ota_satelite_t inicio{};
    inicio.tamanho_firmware = contexto.configuracao.tamanho_bytes;
    std::memcpy(inicio.sha256, contexto.sha256, sizeof(inicio.sha256));
    std::strncpy(inicio.versao, contexto.configuracao.versao,
                 sizeof(inicio.versao) - 1);
    // Um ACK perdido também pode deixar uma sessão aberta no satélite.
    contexto.transferencia_iniciada = true;
    const esp_err_t erro = enviar_e_confirmar(
        contexto, OTA_SATELITE_TIPO_INICIAR, 0, 0, &inicio, sizeof(inicio), 0,
        OTA_SATELITE_FASE_RECEBENDO);
    if (erro == ESP_OK) {
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
    if (contexto.observador != nullptr && obter_erro(contexto) == ESP_OK) {
        const uint32_t decorrido = static_cast<uint32_t>(
            esp_timer_get_time() / 1000 - contexto.inicio_ms);
        contexto.observador(contexto.bytes_confirmados,
                            contexto.configuracao.tamanho_bytes, decorrido);
    }
    return ESP_OK;
}

void tarefa_pipeline(void*) {
    while (true) {
        servico_supervisao_informar_atividade(ModuloSupervisionado::PipelineOta,
            "ota_uart_pipeline", "aguardando bloco", 0);
        BlocoPipeline bloco{};
        if (xQueueReceive(fila_blocos_prontos, &bloco, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (bloco.encerrar_tarefa) break;
        servico_supervisao_informar_atividade(ModuloSupervisionado::PipelineOta,
            "ota_uart_pipeline", "enviando/confirmando bloco", 60000);

        portENTER_CRITICAL(&trava_estado);
        ContextoDownload* contexto = contexto_pipeline;
        portEXIT_CRITICAL(&trava_estado);
        if (bloco.finalizar) {
            if (contexto == nullptr) continue;
            const TaskHandle_t solicitante = contexto->tarefa_solicitante;
            portENTER_CRITICAL(&trava_estado);
            pipeline_drenado = true;
            const bool abandonado = produtor_abandonou_pipeline;
            portEXIT_CRITICAL(&trava_estado);
            if (abandonado) {
                cancelar_no_satelite(*contexto);
                liberar_sessao();
            } else {
                xTaskNotifyGive(solicitante);
            }
            continue;
        }

        if (contexto != nullptr && obter_erro(*contexto) == ESP_OK) {
            const uint8_t* dados = obter_buffer(bloco.indice);
            const esp_err_t erro = dados == nullptr
                                       ? ESP_ERR_INVALID_STATE
                                       : enviar_bloco(*contexto, dados, bloco.tamanho);
            definir_primeiro_erro(*contexto, erro);
        }
        if (bloco.indice < configuracao::QUANTIDADE_BUFFERS_PIPELINE) {
            if (xQueueSend(fila_buffers_livres, &bloco.indice, 0) != pdTRUE &&
                contexto != nullptr) {
                definir_primeiro_erro(*contexto, ESP_ERR_INVALID_STATE);
            }
        }
    }
    tarefa_pipeline_handle = nullptr;
    vTaskDelete(nullptr);
}

esp_err_t adquirir_buffer_livre(ContextoDownload& contexto) {
    const TickType_t inicio = xTaskGetTickCount();
    while (obter_erro(contexto) == ESP_OK) {
        if (cancelamento_ativo(contexto)) return ESP_ERR_INVALID_STATE;
        if (xTaskGetTickCount() - inicio >= TEMPO_LIMITE_PIPELINE) return ESP_ERR_TIMEOUT;
        uint8_t indice = INDICE_BUFFER_INVALIDO;
        if (xQueueReceive(fila_buffers_livres, &indice,
                          INTERVALO_ESPERA_PIPELINE) == pdTRUE) {
            contexto.indice_bloco_atual = indice;
            contexto.ocupacao_bloco = 0;
            return ESP_OK;
        }
    }
    return obter_erro(contexto);
}

esp_err_t enfileirar_bloco_atual(ContextoDownload& contexto) {
    if (contexto.indice_bloco_atual == INDICE_BUFFER_INVALIDO ||
        contexto.ocupacao_bloco == 0 ||
        contexto.ocupacao_bloco > UINT16_MAX) {
        return ESP_ERR_INVALID_STATE;
    }
    const BlocoPipeline bloco{
        contexto.indice_bloco_atual,
        static_cast<uint16_t>(contexto.ocupacao_bloco), false, false};
    if (cancelamento_ativo(contexto)) return ESP_ERR_INVALID_STATE;
    const esp_err_t erro = obter_erro(contexto);
    if (erro != ESP_OK) return erro;
    if (xQueueSend(fila_blocos_prontos, &bloco, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    contexto.indice_bloco_atual = INDICE_BUFFER_INVALIDO;
    contexto.ocupacao_bloco = 0;
    return ESP_OK;
}

esp_err_t abrir_download(ContextoDownload& contexto, esp_http_client_handle_t cliente) {
    for (int redirecionamentos = 0;; ++redirecionamentos) {
        if (cancelamento_ativo(contexto)) return ESP_ERR_INVALID_STATE;
        if (esp_http_client_get_transport_type(cliente) != HTTP_TRANSPORT_OVER_SSL) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const esp_err_t erro_abertura = esp_http_client_open(cliente, 0);
        if (erro_abertura != ESP_OK) return erro_abertura;
        if (cancelamento_ativo(contexto)) return ESP_ERR_INVALID_STATE;

        const int64_t tamanho = esp_http_client_fetch_headers(cliente);
        if (tamanho < 0) return ESP_ERR_HTTP_FETCH_HEADER;
        const int codigo = esp_http_client_get_status_code(cliente);
        if (codigo == 200) {
            if (tamanho > 0 && tamanho != contexto.configuracao.tamanho_bytes) {
                return ESP_ERR_INVALID_SIZE;
            }
            return ESP_OK;
        }
        if (redirecionamentos == configuracao::MAXIMO_REDIRECIONAMENTOS_OTA ||
            (codigo != 301 && codigo != 302 && codigo != 303 &&
             codigo != 307 && codigo != 308)) {
            ESP_LOGW(ETIQUETA, "%s: resposta HTTP %d ao baixar firmware",
                     nome_destino(contexto.configuracao.destino), codigo);
            return ESP_ERR_INVALID_RESPONSE;
        }
        ESP_RETURN_ON_ERROR(esp_http_client_set_redirection(cliente), ETIQUETA, "redirecionamento OTA");
        ESP_RETURN_ON_ERROR(esp_http_client_close(cliente), ETIQUETA, "fechar resposta de redirecionamento");
    }
}

esp_err_t baixar_blocos(ContextoDownload& contexto, esp_http_client_handle_t cliente) {
    esp_err_t erro = abrir_download(contexto, cliente);
    if (erro != ESP_OK) return erro;
    if (cancelamento_ativo(contexto)) return ESP_ERR_INVALID_STATE;
    erro = iniciar_no_satelite(contexto);
    if (erro != ESP_OK) return erro;
    if (cancelamento_ativo(contexto)) return ESP_ERR_INVALID_STATE;
    erro = esp_http_client_set_timeout_ms(cliente, configuracao::TEMPO_LEITURA_HTTP_OTA_MS);
    if (erro != ESP_OK) return erro;

    const uint32_t tamanho_total = contexto.configuracao.tamanho_bytes;
    uint32_t bytes_lidos = 0;
    int64_t ultimo_byte_us = esp_timer_get_time();
    while (bytes_lidos < tamanho_total || !esp_http_client_is_complete_data_received(cliente)) {
        if (cancelamento_ativo(contexto)) return ESP_ERR_INVALID_STATE;
        erro = obter_erro(contexto);
        if (erro != ESP_OK) return erro;

        char excedente = 0;
        char* destino = &excedente;
        size_t capacidade = 1;
        if (bytes_lidos < tamanho_total) {
            if (contexto.indice_bloco_atual == INDICE_BUFFER_INVALIDO) {
                erro = adquirir_buffer_livre(contexto);
                if (erro != ESP_OK) return erro;
                // A espera por buffer livre pertence à UART.
                ultimo_byte_us = esp_timer_get_time();
            }
            auto* bloco = obter_buffer(contexto.indice_bloco_atual);
            if (bloco == nullptr) return ESP_ERR_INVALID_STATE;
            destino = reinterpret_cast<char*>(bloco + contexto.ocupacao_bloco);
            capacidade = std::min<size_t>(
                contexto.configuracao.tamanho_bloco - contexto.ocupacao_bloco,
                tamanho_total - bytes_lidos);
        }

        const int lidos = esp_http_client_read(cliente, destino, static_cast<int>(capacidade));
        if (cancelamento_ativo(contexto)) return ESP_ERR_INVALID_STATE;
        if (lidos > 0) {
            if (destino == &excedente) return ESP_ERR_INVALID_SIZE;
            ultimo_byte_us = esp_timer_get_time();
            bytes_lidos += static_cast<uint32_t>(lidos);
            contexto.ocupacao_bloco += static_cast<size_t>(lidos);
            if (contexto.ocupacao_bloco == contexto.configuracao.tamanho_bloco ||
                bytes_lidos == tamanho_total) {
                erro = enfileirar_bloco_atual(contexto);
                if (erro != ESP_OK) return erro;
            }
            continue;
        }
        if (esp_http_client_is_complete_data_received(cliente)) {
            return bytes_lidos == tamanho_total ? ESP_OK : ESP_ERR_HTTP_INCOMPLETE_DATA;
        }
        if (lidos != -ESP_ERR_HTTP_EAGAIN) return ESP_ERR_HTTP_INCOMPLETE_DATA;
        if (esp_timer_get_time() - ultimo_byte_us >=
            configuracao::TEMPO_INATIVIDADE_HTTP_OTA_MS * 1000LL) {
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}
void devolver_buffer_atual(ContextoDownload& contexto) {
    if (contexto.indice_bloco_atual == INDICE_BUFFER_INVALIDO) return;
    (void)xQueueSend(fila_buffers_livres, &contexto.indice_bloco_atual, 0);
    contexto.indice_bloco_atual = INDICE_BUFFER_INVALIDO;
    contexto.ocupacao_bloco = 0;
}

bool finalizar_pipeline(ContextoDownload& contexto) {
    devolver_buffer_atual(contexto);
    // A fila tem um slot reservado para este marcador além dos buffers de dados.
    const BlocoPipeline fim{INDICE_BUFFER_INVALIDO, 0, true, false};
    if (xQueueSend(fila_blocos_prontos, &fim, 0) != pdTRUE) {
        definir_primeiro_erro(contexto, ESP_ERR_INVALID_STATE);
        return false;
    }

    const TickType_t inicio = xTaskGetTickCount();
    while (true) {
        portENTER_CRITICAL(&trava_estado);
        if (pipeline_drenado) {
            portEXIT_CRITICAL(&trava_estado);
            return true;
        }
        const TickType_t decorrido = xTaskGetTickCount() - inicio;
        if (decorrido >= TEMPO_LIMITE_PIPELINE) {
            // O consumidor ainda é dono do contexto. Ele cancela no satélite
            // e devolve a sessão quando terminar o último bloco.
            produtor_abandonou_pipeline = true;
            if (contexto.erro == ESP_OK) contexto.erro = ESP_ERR_TIMEOUT;
            portEXIT_CRITICAL(&trava_estado);
            ESP_LOGW(ETIQUETA, "Encerramento OTA ainda pendente; aguardando a tarefa UART");
            return false;
        }
        portEXIT_CRITICAL(&trava_estado);
        (void)ulTaskNotifyTake(pdTRUE, TEMPO_LIMITE_PIPELINE - decorrido);
    }
}

void preparar_pipeline(ContextoDownload& contexto) {
    xQueueReset(fila_buffers_livres);
    xQueueReset(fila_blocos_prontos);
    for (uint8_t indice = 0; indice < configuracao::QUANTIDADE_BUFFERS_PIPELINE; ++indice) {
        (void)xQueueSend(fila_buffers_livres, &indice, 0);
    }
    (void)ulTaskNotifyTake(pdTRUE, 0);
    contexto.indice_bloco_atual = INDICE_BUFFER_INVALIDO;
    contexto.ocupacao_bloco = 0;
    contexto.tarefa_solicitante = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&trava_estado);
    pipeline_drenado = false;
    produtor_abandonou_pipeline = false;
    contexto_pipeline = &contexto;
    portEXIT_CRITICAL(&trava_estado);
}

void cancelar_no_satelite(ContextoDownload& contexto) {
    if (!contexto.transferencia_iniciada) return;
    const size_t tamanho = protocolo_ota_satelites_montar_quadro(
        contexto.quadro, OTA_SATELITE_TAMANHO_MAXIMO_QUADRO, OTA_SATELITE_TIPO_CANCELAR,
        contexto.sessao, contexto.sequencia, contexto.bytes_confirmados, nullptr, 0);
    if (tamanho != 0) {
        (void)gerenciador_uart_enviar(
            contexto.configuracao.destino, contexto.quadro, tamanho,
            pdMS_TO_TICKS(100));
    }
}

void liberar_recursos_pipeline() {
    if (fila_blocos_prontos != nullptr) {
        vQueueDelete(fila_blocos_prontos);
        fila_blocos_prontos = nullptr;
    }
    if (fila_buffers_livres != nullptr) {
        vQueueDelete(fila_buffers_livres);
        fila_buffers_livres = nullptr;
    }
    if (fila_respostas != nullptr) {
        vQueueDelete(fila_respostas);
        fila_respostas = nullptr;
    }
    if (memoria_pipeline != nullptr) {
        heap_caps_free(memoria_pipeline);
        memoria_pipeline = nullptr;
        pipeline_na_psram = false;
    }
    pipeline_operacional = false;
}
}  // namespace

esp_err_t transporte_ota_satelites_iniciar() {
    if (fila_respostas != nullptr && fila_buffers_livres != nullptr &&
        fila_blocos_prontos != nullptr && memoria_pipeline != nullptr &&
        tarefa_pipeline_handle != nullptr) {
        return ESP_OK;
    }

    fila_respostas = xQueueCreate(8, sizeof(RespostaRecebida));
    fila_buffers_livres = xQueueCreate(configuracao::QUANTIDADE_BUFFERS_PIPELINE,
                                       sizeof(uint8_t));
    fila_blocos_prontos = xQueueCreate(configuracao::QUANTIDADE_BUFFERS_PIPELINE + 1,
                                       sizeof(BlocoPipeline));
    const size_t tamanho_memoria = TAMANHO_BLOCOS_PIPELINE + OTA_SATELITE_TAMANHO_MAXIMO_QUADRO;
    memoria_pipeline = static_cast<uint8_t*>(heap_caps_malloc(
        tamanho_memoria, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    pipeline_na_psram = memoria_pipeline != nullptr;
    if (memoria_pipeline == nullptr) {
        ESP_LOGW(ETIQUETA,
                 "PSRAM indisponível para o pipeline OTA; usando RAM interna");
        memoria_pipeline = static_cast<uint8_t*>(
            heap_caps_malloc(tamanho_memoria, MALLOC_CAP_8BIT));
    }
    if (fila_respostas == nullptr || fila_buffers_livres == nullptr ||
        fila_blocos_prontos == nullptr || memoria_pipeline == nullptr) {
        liberar_recursos_pipeline();
        return ESP_ERR_NO_MEM;
    }
    if (configuracao::criar_tarefa(tarefa_pipeline, "ota_uart_pipeline", nullptr, &tarefa_pipeline_handle) != pdPASS) {
        tarefa_pipeline_handle = nullptr;
        liberar_recursos_pipeline();
        return ESP_ERR_NO_MEM;
    }
    pipeline_operacional = true;
    ESP_LOGI(ETIQUETA,
             "Pipeline OTA pronto: %u buffers de %u bytes e quadro UART em %s",
             static_cast<unsigned>(configuracao::QUANTIDADE_BUFFERS_PIPELINE),
             static_cast<unsigned>(OTA_SATELITE_TAMANHO_MAXIMO_CARGA),
             pipeline_na_psram ? "PSRAM" : "RAM interna");
    return ESP_OK;
}

esp_err_t transporte_ota_satelites_encerrar() {
    portENTER_CRITICAL(&trava_estado);
    const bool ocupado = transferencia_em_andamento;
    portEXIT_CRITICAL(&trava_estado);
    if (ocupado) return ESP_ERR_INVALID_STATE;

    if (tarefa_pipeline_handle != nullptr && fila_blocos_prontos != nullptr) {
        const BlocoPipeline encerrar{INDICE_BUFFER_INVALIDO, 0, false, true};
        if (xQueueSend(fila_blocos_prontos, &encerrar,
                       pdMS_TO_TICKS(250)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
        const TickType_t inicio = xTaskGetTickCount();
        while (tarefa_pipeline_handle != nullptr &&
               xTaskGetTickCount() - inicio < pdMS_TO_TICKS(1000)) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (tarefa_pipeline_handle != nullptr) return ESP_ERR_TIMEOUT;
    }
    liberar_recursos_pipeline();
    return ESP_OK;
}

esp_err_t transporte_ota_satelites_executar(
    const ConfiguracaoTransferenciaOtaSatelite& configuracao,
    ObservadorProgressoOtaSatelite observador,
    bool (*cancelamento_solicitado)(), bool (*autorizar_finalizacao)(),
    bool& finalizacao_incerta) {
    finalizacao_incerta = false;
    if (cancelamento_solicitado == nullptr || autorizar_finalizacao == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const bool destino_valido = configuracao.destino == DestinoUart::Equipe ||
                                configuracao.destino == DestinoUart::Visitantes;
    if (fila_respostas == nullptr || fila_buffers_livres == nullptr ||
        fila_blocos_prontos == nullptr || memoria_pipeline == nullptr ||
        tarefa_pipeline_handle == nullptr || !pipeline_operacional ||
        configuracao.endereco_https == nullptr || configuracao.versao == nullptr ||
        configuracao.sha256 == nullptr || configuracao.versao[0] == '\0' ||
        std::strlen(configuracao.versao) >= OTA_SATELITE_TAMANHO_VERSAO ||
        configuracao.tamanho_bytes == 0 || configuracao.tempo_limite_http_ms == 0 ||
        (configuracao.tamanho_bloco != OTA_SATELITE_TAMANHO_CARGA_COMPATIVEL &&
         configuracao.tamanho_bloco != OTA_SATELITE_TAMANHO_MAXIMO_CARGA) ||
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

    contexto_download = {};
    ContextoDownload& contexto = contexto_download;
    contexto.quadro = memoria_pipeline + TAMANHO_BLOCOS_PIPELINE;
    contexto.configuracao = configuracao;
    contexto.observador = observador;
    contexto.cancelamento_solicitado = cancelamento_solicitado;
    contexto.sessao = esp_random();
    if (contexto.sessao == 0) contexto.sessao = 1;
    contexto.sequencia = 1;
    contexto.erro = gerenciador_ota_converter_sha256(
                         configuracao.sha256, contexto.sha256)
                         ? ESP_OK : ESP_ERR_INVALID_ARG;
    limpar_respostas_pendentes();
    preparar_pipeline(contexto);

    esp_http_client_config_t http{};
    http.url = configuracao.endereco_https;
    http.disable_auto_redirect = true;
    http.crt_bundle_attach = esp_crt_bundle_attach;
    http.timeout_ms = static_cast<int>(configuracao.tempo_limite_http_ms);
    http.buffer_size = configuracao.tamanho_bloco;
    http.buffer_size_tx = 2048;
    http.keep_alive_enable = true;
    http.user_agent = "UFSM-Carro-OTA-Satelites/2";

    esp_http_client_handle_t cliente = obter_erro(contexto) == ESP_OK
                                           ? esp_http_client_init(&http) : nullptr;
    if (cliente == nullptr && obter_erro(contexto) == ESP_OK) {
        definir_primeiro_erro(contexto, ESP_ERR_NO_MEM);
    }
    if (obter_erro(contexto) == ESP_OK) {
        ESP_LOGW(ETIQUETA,
                 "%s: pipeline HTTPS/PSRAM/UART ativo para firmware %s em blocos de %u bytes",
                 nome_destino(configuracao.destino), configuracao.versao,
                 static_cast<unsigned>(configuracao.tamanho_bloco));
        definir_primeiro_erro(contexto, baixar_blocos(contexto, cliente));
    }
    if (cliente != nullptr) esp_http_client_cleanup(cliente);

    if (!finalizar_pipeline(contexto)) return ESP_ERR_TIMEOUT;
    if (obter_erro(contexto) == ESP_OK &&
        contexto.bytes_confirmados != configuracao.tamanho_bytes) {
        definir_primeiro_erro(contexto, ESP_ERR_INVALID_SIZE);
    }
    if (obter_erro(contexto) == ESP_OK) {
        if (autorizar_finalizacao()) {
            definir_primeiro_erro(contexto, enviar_e_confirmar(
                contexto, OTA_SATELITE_TIPO_FINALIZAR, contexto.sequencia,
                contexto.bytes_confirmados, nullptr, 0, contexto.bytes_confirmados,
                OTA_SATELITE_FASE_PRONTO_PARA_REINICIAR, &finalizacao_incerta));
        } else {
            definir_primeiro_erro(contexto, ESP_ERR_INVALID_STATE);
        }
    }
    const esp_err_t erro = obter_erro(contexto);
    if (erro != ESP_OK && !finalizacao_incerta) cancelar_no_satelite(contexto);
    liberar_sessao();
    return erro;
}

void transporte_ota_satelites_registrar_resposta(DestinoUart origem,
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
