// Encadeia download HTTPS e envio UART usando buffers fixos reutilizáveis.
#include "nucleo/configuracao_tarefas.h"
#include "servicos/servico_supervisao.h"
#include "servicos/transporte_ota_satelites.h"

#include <algorithm>
#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gerenciadores/gerenciador_ota.h"
#include "gerenciadores/gerenciador_uart.h"
#include "nucleo/configuracao_placa.h"
#include "protocolo_ota_satelites.h"

namespace {
constexpr char ETIQUETA[] = "ota_uart";
constexpr uint32_t TEMPO_RESPOSTA_MS = 1500;
constexpr uint32_t TEMPO_INICIO_MS = 10000;
constexpr uint32_t TEMPO_FINALIZACAO_MS = 15000;
constexpr unsigned MAXIMO_TENTATIVAS = 4;
constexpr uint8_t QUANTIDADE_BUFFERS_PIPELINE = 3;
constexpr uint8_t INDICE_BUFFER_INVALIDO = UINT8_MAX;
constexpr TickType_t INTERVALO_ESPERA_PIPELINE = pdMS_TO_TICKS(100);
constexpr TickType_t TEMPO_LIMITE_PIPELINE = pdMS_TO_TICKS(30000);

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
    uint8_t quadro[OTA_SATELITE_TAMANHO_MAXIMO_QUADRO];
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
bool cancelamento_solicitado = false;
bool pipeline_operacional = false;
portMUX_TYPE trava_estado = portMUX_INITIALIZER_UNLOCKED;

const char* nome_destino(DestinoUart destino) {
    return destino == DestinoUart::Equipe ? "equipe" : "visitantes";
}

uint8_t* obter_buffer(uint8_t indice) {
    if (memoria_pipeline == nullptr || indice >= QUANTIDADE_BUFFERS_PIPELINE) {
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

bool cancelamento_ativo() {
    portENTER_CRITICAL(&trava_estado);
    const bool solicitado = cancelamento_solicitado;
    portEXIT_CRITICAL(&trava_estado);
    return solicitado;
}

void invalidar_pipeline() {
    portENTER_CRITICAL(&trava_estado);
    pipeline_operacional = false;
    portEXIT_CRITICAL(&trava_estado);
    ESP_LOGE(ETIQUETA,
             "Pipeline OTA bloqueado após timeout; nova transferência requer reinicialização segura");
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
    const size_t tamanho = protocolo_ota_satelites_montar_quadro(
        contexto.quadro, sizeof(contexto.quadro), tipo, contexto.sessao, sequencia,
        deslocamento, carga, tamanho_carga);
    if (tamanho == 0) return ESP_ERR_INVALID_SIZE;

    for (unsigned tentativa = 1; tentativa <= MAXIMO_TENTATIVAS; ++tentativa) {
        if (cancelamento_ativo()) return ESP_ERR_INVALID_STATE;
        const esp_err_t erro_envio = gerenciador_uart_enviar(
            contexto.configuracao.destino, contexto.quadro, tamanho,
            pdMS_TO_TICKS(250));
        if (erro_envio != ESP_OK) {
            ESP_LOGW(ETIQUETA, "%s: tentativa %u/%u falhou ao transmitir bloco %lu: %s",
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
            if (cancelamento_ativo()) return ESP_ERR_INVALID_STATE;
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
            if (contexto != nullptr && contexto->tarefa_solicitante != nullptr) {
                xTaskNotifyGive(contexto->tarefa_solicitante);
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
        if (bloco.indice < QUANTIDADE_BUFFERS_PIPELINE) {
            if (xQueueSend(fila_buffers_livres, &bloco.indice,
                           TEMPO_LIMITE_PIPELINE) != pdTRUE &&
                contexto != nullptr) {
                definir_primeiro_erro(*contexto, ESP_ERR_TIMEOUT);
                invalidar_pipeline();
            }
        }
    }
    tarefa_pipeline_handle = nullptr;
    vTaskDelete(nullptr);
}

esp_err_t adquirir_buffer_livre(ContextoDownload& contexto) {
    while (obter_erro(contexto) == ESP_OK) {
        if (cancelamento_ativo()) return ESP_ERR_INVALID_STATE;
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
    while (obter_erro(contexto) == ESP_OK) {
        if (cancelamento_ativo()) return ESP_ERR_INVALID_STATE;
        if (xQueueSend(fila_blocos_prontos, &bloco,
                       INTERVALO_ESPERA_PIPELINE) == pdTRUE) {
            contexto.indice_bloco_atual = INDICE_BUFFER_INVALIDO;
            contexto.ocupacao_bloco = 0;
            return ESP_OK;
        }
    }
    return obter_erro(contexto);
}

esp_err_t tratar_evento_http(esp_http_client_event_t* evento) {
    if (evento == nullptr || evento->user_data == nullptr) return ESP_ERR_INVALID_ARG;
    auto& contexto = *static_cast<ContextoDownload*>(evento->user_data);
    if (cancelamento_ativo()) {
        definir_primeiro_erro(contexto, ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    if (evento->event_id != HTTP_EVENT_ON_DATA || evento->data == nullptr ||
        evento->data_len <= 0 || obter_erro(contexto) != ESP_OK) {
        return obter_erro(contexto);
    }

    if (!contexto.transferencia_iniciada) {
        const int codigo_http = esp_http_client_get_status_code(evento->client);
        if (codigo_http >= 300 && codigo_http < 400) return ESP_OK;
        if (codigo_http != 200) {
            definir_primeiro_erro(contexto, ESP_ERR_HTTP_BASE);
            return obter_erro(contexto);
        }
        definir_primeiro_erro(contexto, iniciar_no_satelite(contexto));
        if (obter_erro(contexto) != ESP_OK) return obter_erro(contexto);
    }

    const uint8_t* dados = static_cast<const uint8_t*>(evento->data);
    size_t restantes = static_cast<size_t>(evento->data_len);
    while (restantes > 0 && obter_erro(contexto) == ESP_OK) {
        if (contexto.indice_bloco_atual == INDICE_BUFFER_INVALIDO) {
            definir_primeiro_erro(contexto, adquirir_buffer_livre(contexto));
            if (obter_erro(contexto) != ESP_OK) break;
        }
        uint8_t* destino = obter_buffer(contexto.indice_bloco_atual);
        if (destino == nullptr) {
            definir_primeiro_erro(contexto, ESP_ERR_INVALID_STATE);
            break;
        }
        const size_t espaco = contexto.configuracao.tamanho_bloco -
                              contexto.ocupacao_bloco;
        const size_t copiar = restantes < espaco ? restantes : espaco;
        std::memcpy(destino + contexto.ocupacao_bloco, dados, copiar);
        contexto.ocupacao_bloco += copiar;
        dados += copiar;
        restantes -= copiar;
        if (contexto.ocupacao_bloco == contexto.configuracao.tamanho_bloco) {
            definir_primeiro_erro(contexto, enfileirar_bloco_atual(contexto));
        }
    }
    return obter_erro(contexto);
}

void devolver_buffer_atual(ContextoDownload& contexto) {
    if (contexto.indice_bloco_atual == INDICE_BUFFER_INVALIDO) return;
    (void)xQueueSend(fila_buffers_livres, &contexto.indice_bloco_atual, 0);
    contexto.indice_bloco_atual = INDICE_BUFFER_INVALIDO;
    contexto.ocupacao_bloco = 0;
}

esp_err_t finalizar_pipeline(ContextoDownload& contexto) {
    if (obter_erro(contexto) == ESP_OK && contexto.ocupacao_bloco > 0) {
        definir_primeiro_erro(contexto, enfileirar_bloco_atual(contexto));
    } else {
        devolver_buffer_atual(contexto);
    }
    const BlocoPipeline fim{INDICE_BUFFER_INVALIDO, 0, true, false};
    if (xQueueSend(fila_blocos_prontos, &fim,
                   TEMPO_LIMITE_PIPELINE) != pdTRUE) {
        definir_primeiro_erro(contexto, ESP_ERR_TIMEOUT);
        invalidar_pipeline();
        return obter_erro(contexto);
    }
    if (ulTaskNotifyTake(pdTRUE, TEMPO_LIMITE_PIPELINE) == 0) {
        definir_primeiro_erro(contexto, ESP_ERR_TIMEOUT);
        invalidar_pipeline();
    }
    return obter_erro(contexto);
}

void preparar_pipeline(ContextoDownload& contexto) {
    xQueueReset(fila_buffers_livres);
    xQueueReset(fila_blocos_prontos);
    for (uint8_t indice = 0; indice < QUANTIDADE_BUFFERS_PIPELINE; ++indice) {
        (void)xQueueSend(fila_buffers_livres, &indice, 0);
    }
    (void)ulTaskNotifyTake(pdTRUE, 0);
    contexto.indice_bloco_atual = INDICE_BUFFER_INVALIDO;
    contexto.ocupacao_bloco = 0;
    contexto.tarefa_solicitante = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&trava_estado);
    contexto_pipeline = &contexto;
    portEXIT_CRITICAL(&trava_estado);
}

void cancelar_no_satelite(ContextoDownload& contexto) {
    if (!contexto.transferencia_iniciada) return;
    const size_t tamanho = protocolo_ota_satelites_montar_quadro(
        contexto.quadro, sizeof(contexto.quadro), OTA_SATELITE_TIPO_CANCELAR,
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
    fila_buffers_livres = xQueueCreate(QUANTIDADE_BUFFERS_PIPELINE,
                                       sizeof(uint8_t));
    fila_blocos_prontos = xQueueCreate(QUANTIDADE_BUFFERS_PIPELINE + 1,
                                       sizeof(BlocoPipeline));
    const size_t tamanho_memoria =
        static_cast<size_t>(QUANTIDADE_BUFFERS_PIPELINE) *
        OTA_SATELITE_TAMANHO_MAXIMO_CARGA;
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
             "Pipeline OTA pronto: %u buffers fixos de %u bytes em %s",
             static_cast<unsigned>(QUANTIDADE_BUFFERS_PIPELINE),
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
        const TickType_t limite = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
        while (tarefa_pipeline_handle != nullptr &&
               xTaskGetTickCount() < limite) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (tarefa_pipeline_handle != nullptr) return ESP_ERR_TIMEOUT;
    }
    liberar_recursos_pipeline();
    return ESP_OK;
}

esp_err_t transporte_ota_satelites_solicitar_cancelamento() {
    portENTER_CRITICAL(&trava_estado);
    if (!transferencia_em_andamento) {
        portEXIT_CRITICAL(&trava_estado);
        return ESP_ERR_INVALID_STATE;
    }
    cancelamento_solicitado = true;
    portEXIT_CRITICAL(&trava_estado);
    return ESP_OK;
}

esp_err_t transporte_ota_satelites_executar(
    const ConfiguracaoTransferenciaOtaSatelite& configuracao,
    ObservadorProgressoOtaSatelite observador) {
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
    cancelamento_solicitado = false;
    portEXIT_CRITICAL(&trava_estado);

    contexto_download = {};
    ContextoDownload& contexto = contexto_download;
    contexto.configuracao = configuracao;
    contexto.observador = observador;
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
    http.event_handler = tratar_evento_http;
    http.user_data = &contexto;
    http.crt_bundle_attach = esp_crt_bundle_attach;
    http.timeout_ms = static_cast<int>(configuracao.tempo_limite_http_ms);
    http.buffer_size = configuracao.tamanho_bloco;
    http.buffer_size_tx = 2048;
    http.max_redirection_count = 8;
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
        const esp_err_t erro_http = esp_http_client_perform(cliente);
        definir_primeiro_erro(contexto, erro_http);
        const int codigo_http = esp_http_client_get_status_code(cliente);
        if (obter_erro(contexto) == ESP_OK && codigo_http != 200) {
            definir_primeiro_erro(contexto, ESP_ERR_HTTP_BASE);
        }
    }
    if (cliente != nullptr) esp_http_client_cleanup(cliente);

    (void)finalizar_pipeline(contexto);
    if (obter_erro(contexto) == ESP_OK &&
        contexto.bytes_confirmados != configuracao.tamanho_bytes) {
        definir_primeiro_erro(contexto, ESP_ERR_INVALID_SIZE);
    }
    if (obter_erro(contexto) == ESP_OK) {
        definir_primeiro_erro(contexto, enviar_e_confirmar(
            contexto, OTA_SATELITE_TIPO_FINALIZAR, contexto.sequencia,
            contexto.bytes_confirmados, nullptr, 0, contexto.bytes_confirmados,
            OTA_SATELITE_FASE_PRONTO_PARA_REINICIAR));
    }
    if (obter_erro(contexto) != ESP_OK) cancelar_no_satelite(contexto);

    portENTER_CRITICAL(&trava_estado);
    contexto_pipeline = nullptr;
    transferencia_em_andamento = false;
    cancelamento_solicitado = false;
    portEXIT_CRITICAL(&trava_estado);
    return obter_erro(contexto);
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
