// Transporta comandos UART, correlaciona respostas e preserva a ordem de execução.
#include "servicos/comunicacao/comandos_uart.h"

#include <cstring>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gerenciadores/uart.h"
#include "nucleo/configuracao/placa.h"
#include "nucleo/configuracao/tarefas.h"
#include "nucleo/configuracao/comunicacao.h"

namespace {
constexpr char ETIQUETA[] = "servico_comandos";
static_assert(
    COMANDO_TAMANHO_MAXIMO_QUADRO <=
        configuracao::TAMANHO_MAXIMO_MENSAGEM_UART_ASSINCRONA,
    "O quadro de comando excede o limite da fila UART assincrona");

struct RespostaPendente {
    DestinoUart origem;
    cabecalho_quadro_comando_t cabecalho;
    uint8_t carga[COMANDO_TAMANHO_MAXIMO_CARGA];
};

struct RespostaRepetivel {
    uint32_t solicitacao = 0;
    uint16_t comando = 0;
    size_t tamanho = 0;
    uint8_t quadro[COMANDO_TAMANHO_MAXIMO_QUADRO]{};
};

struct SolicitacaoOperacionalPendente {
    DestinoUart origem;
    cabecalho_quadro_comando_t cabecalho;
    int64_t recebido_em_us;
    uint8_t carga[COMANDO_TAMANHO_MAXIMO_CARGA];
};

struct SolicitacaoEmAndamento {
    uint32_t solicitacao = 0;
    uint16_t comando = 0;
    bool ativa = false;
};

struct RecepcaoRearme {
    uint32_t solicitacao = 0;
    int64_t recebido_em_us = 0;
};

QueueHandle_t fila_respostas = nullptr;
QueueHandle_t fila_operacoes = nullptr;
SemaphoreHandle_t mutex_solicitacao = nullptr;
TaskHandle_t tarefa_operacoes = nullptr;
portMUX_TYPE trava_estatisticas = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE trava_repeticoes = portMUX_INITIALIZER_UNLOCKED;
EstatisticasServicoComandos estatisticas{};
RespostaRepetivel respostas_repetiveis[2]{};
SolicitacaoEmAndamento solicitacoes_em_andamento[2]{};
RecepcaoRearme rearmes_recentes[4]{};
size_t proximo_rearme_recente = 0;
DespachoComando despachar_comando = nullptr;

uint8_t no_para_destino(DestinoUart destino) {
    return destino == DestinoUart::Equipe ? NO_COMANDO_EQUIPE
                                          : NO_COMANDO_VISITANTES;
}

size_t indice_destino(DestinoUart destino) {
    return destino == DestinoUart::Equipe ? 0u : 1u;
}

void registrar_erro(esp_err_t erro) {
    portENTER_CRITICAL(&trava_estatisticas);
    estatisticas.ultimo_erro = erro;
    portEXIT_CRITICAL(&trava_estatisticas);
}

void processar_solicitacao_agora(DestinoUart origem,
                                 const cabecalho_quadro_comando_t& cabecalho,
                                 const uint8_t* carga, int64_t recebido_em_us) {
    ESP_LOGI(ETIQUETA, "Comando recebido de %s: código=%u solicitação=%lu",
             origem == DestinoUart::Equipe ? "equipe" : "visitantes",
             static_cast<unsigned>(cabecalho.comando),
             static_cast<unsigned long>(cabecalho.solicitacao));
    const size_t indice = indice_destino(origem);
    portENTER_CRITICAL(&trava_repeticoes);
    const bool repetida = respostas_repetiveis[indice].solicitacao ==
                              cabecalho.solicitacao &&
                          respostas_repetiveis[indice].comando ==
                              cabecalho.comando;
    const size_t tamanho_repetido = respostas_repetiveis[indice].tamanho;
    uint8_t quadro_repetido[COMANDO_TAMANHO_MAXIMO_QUADRO];
    if (repetida && tamanho_repetido > 0)
        std::memcpy(quadro_repetido, respostas_repetiveis[indice].quadro,
                    tamanho_repetido);
    portEXIT_CRITICAL(&trava_repeticoes);
    if (repetida && tamanho_repetido > 0) {
        const esp_err_t erro =
            gerenciador_uart_enfileirar(origem, quadro_repetido, tamanho_repetido);
        portENTER_CRITICAL(&trava_estatisticas);
        estatisticas.repeticoes++;
        if (erro == ESP_OK) {
            estatisticas.respostas_enviadas++;
            estatisticas.ultimo_erro = ESP_OK;
        } else {
            estatisticas.ultimo_erro = erro;
        }
        portEXIT_CRITICAL(&trava_estatisticas);
        return;
    }

    uint8_t carga_resposta[COMANDO_TAMANHO_MAXIMO_CARGA]{};
    uint16_t tamanho_resposta = 0;
    codigo_resposta_comando_t resultado = RESPOSTA_COMANDO_ERRO_INTERNO;
    if (despachar_comando != nullptr) {
        resultado = despachar_comando(
            cabecalho.comando, origem, carga, cabecalho.tamanho_carga,
            carga_resposta, &tamanho_resposta, recebido_em_us);
    }
    uint8_t quadro[COMANDO_TAMANHO_MAXIMO_QUADRO];
    const size_t tamanho = protocolo_comandos_montar_quadro(
        quadro, sizeof(quadro), TIPO_QUADRO_COMANDO_RESPOSTA,
        NO_COMANDO_MESTRE, cabecalho.origem, cabecalho.comando,
        cabecalho.solicitacao, resultado, carga_resposta, tamanho_resposta);
    if (tamanho != 0) {
        // A resposta é memorizada antes do envio: se o ACK se perder, a
        // retransmissão não executará novamente uma ação com efeito colateral.
        portENTER_CRITICAL(&trava_repeticoes);
        respostas_repetiveis[indice].solicitacao = cabecalho.solicitacao;
        respostas_repetiveis[indice].comando = cabecalho.comando;
        respostas_repetiveis[indice].tamanho = tamanho;
        std::memcpy(respostas_repetiveis[indice].quadro, quadro, tamanho);
        portEXIT_CRITICAL(&trava_repeticoes);
    }
    const esp_err_t erro = tamanho == 0 ? ESP_ERR_INVALID_SIZE
        : gerenciador_uart_enfileirar(origem, quadro, tamanho);
    portENTER_CRITICAL(&trava_estatisticas);
    estatisticas.solicitacoes_recebidas++;
    if (erro == ESP_OK) {
        estatisticas.respostas_enviadas++;
        estatisticas.ultimo_erro = ESP_OK;
    } else {
        estatisticas.ultimo_erro = erro;
    }
    portEXIT_CRITICAL(&trava_estatisticas);
    ESP_LOGI(ETIQUETA,
             "Resposta ao comando %u enviada para %s: resultado=%u transporte=%s",
             static_cast<unsigned>(cabecalho.comando),
             origem == DestinoUart::Equipe ? "equipe" : "visitantes",
             static_cast<unsigned>(resultado), esp_err_to_name(erro));
}

void tarefa_operacoes_mestre(void*) {
    SolicitacaoOperacionalPendente solicitacao{};
    while (true) {
        if (xQueueReceive(fila_operacoes, &solicitacao, portMAX_DELAY) !=
            pdTRUE) continue;
        processar_solicitacao_agora(solicitacao.origem,
                                    solicitacao.cabecalho,
                                    solicitacao.carga, solicitacao.recebido_em_us);
        const size_t indice = indice_destino(solicitacao.origem);
        portENTER_CRITICAL(&trava_repeticoes);
        if (solicitacoes_em_andamento[indice].solicitacao ==
                solicitacao.cabecalho.solicitacao &&
            solicitacoes_em_andamento[indice].comando ==
                solicitacao.cabecalho.comando) {
            solicitacoes_em_andamento[indice] = {};
        }
        portEXIT_CRITICAL(&trava_repeticoes);
    }
}

int64_t instante_primeiro_rearme(uint32_t solicitacao, int64_t recebido_em_us) {
    portENTER_CRITICAL(&trava_repeticoes);
    for (const auto& anterior : rearmes_recentes) {
        if (anterior.recebido_em_us != 0 && anterior.solicitacao == solicitacao) {
            const int64_t primeiro = anterior.recebido_em_us;
            portEXIT_CRITICAL(&trava_repeticoes);
            return primeiro;
        }
    }
    rearmes_recentes[proximo_rearme_recente] = {solicitacao, recebido_em_us};
    proximo_rearme_recente = (proximo_rearme_recente + 1) %
        (sizeof(rearmes_recentes) / sizeof(rearmes_recentes[0]));
    portEXIT_CRITICAL(&trava_repeticoes);
    return recebido_em_us;
}

void processar_solicitacao(DestinoUart origem,
                           const cabecalho_quadro_comando_t& cabecalho,
                           const uint8_t* carga) {
    int64_t recebido_em_us = esp_timer_get_time();
    if (cabecalho.comando == COMANDO_EXECUTAR_ACAO_MESTRE &&
        cabecalho.tamanho_carga == sizeof(solicitacao_acao_mestre_t)) {
        solicitacao_acao_mestre_t acao_urgente{};
        std::memcpy(&acao_urgente, carga, sizeof(acao_urgente));
        if (acao_urgente.acao == ACAO_MESTRE_DEFINIR_PARADA_REMOTA &&
            acao_urgente.valor == 1) {
            // A parada não espera ações de Wi-Fi, OTA ou Google na fila.
            processar_solicitacao_agora(origem, cabecalho, carga, recebido_em_us);
            return;
        }
        if (origem == DestinoUart::Equipe &&
            acao_urgente.acao == ACAO_MESTRE_DEFINIR_PARADA_REMOTA &&
            (acao_urgente.valor == 0 || acao_urgente.valor == CONTROLE_REMOTO_ARMAR_DAC_ISOLADO)) {
            recebido_em_us = instante_primeiro_rearme(cabecalho.solicitacao, recebido_em_us);
        }
    }
    if (cabecalho.comando != COMANDO_EXECUTAR_ACAO_MESTRE &&
        cabecalho.comando != COMANDO_CONSULTAR_SATELITE &&
        cabecalho.comando != COMANDO_GERIR_WIFI_SATELITE &&
        cabecalho.comando != COMANDO_AJUSTAR_RADIO_SATELITE) {
        processar_solicitacao_agora(origem, cabecalho, carga, recebido_em_us);
        return;
    }
    SolicitacaoOperacionalPendente pendente{};
    pendente.origem = origem;
    pendente.cabecalho = cabecalho;
    pendente.recebido_em_us = recebido_em_us;
    if (cabecalho.tamanho_carga != 0) {
        std::memcpy(pendente.carga, carga, cabecalho.tamanho_carga);
    }
    const size_t indice = indice_destino(origem);
    portENTER_CRITICAL(&trava_repeticoes);
    const bool ja_em_andamento =
        solicitacoes_em_andamento[indice].ativa &&
        solicitacoes_em_andamento[indice].solicitacao == cabecalho.solicitacao &&
        solicitacoes_em_andamento[indice].comando == cabecalho.comando;
    if (!ja_em_andamento) {
        solicitacoes_em_andamento[indice].solicitacao = cabecalho.solicitacao;
        solicitacoes_em_andamento[indice].comando = cabecalho.comando;
        solicitacoes_em_andamento[indice].ativa = true;
    }
    portEXIT_CRITICAL(&trava_repeticoes);
    if (ja_em_andamento) {
        portENTER_CRITICAL(&trava_estatisticas);
        estatisticas.repeticoes++;
        portEXIT_CRITICAL(&trava_estatisticas);
        return;
    }
    if (fila_operacoes == nullptr ||
        xQueueSend(fila_operacoes, &pendente, 0) != pdTRUE) {
        portENTER_CRITICAL(&trava_repeticoes);
        solicitacoes_em_andamento[indice] = {};
        portEXIT_CRITICAL(&trava_repeticoes);
        ESP_LOGW(ETIQUETA,
                 "Fila de ações operacionais cheia; solicitação %lu não enfileirada",
                 static_cast<unsigned long>(cabecalho.solicitacao));
        registrar_erro(ESP_ERR_TIMEOUT);
    }
}
}  // namespace

void servico_comandos_definir_despacho(DespachoComando despacho) {
    despachar_comando = despacho;
}

esp_err_t servico_comandos_iniciar() {
    if (fila_respostas != nullptr && fila_operacoes != nullptr &&
        mutex_solicitacao != nullptr && tarefa_operacoes != nullptr) {
        return ESP_OK;
    }
    fila_respostas = xQueueCreate(8, sizeof(RespostaPendente));
    fila_operacoes = xQueueCreate(4, sizeof(SolicitacaoOperacionalPendente));
    mutex_solicitacao = xSemaphoreCreateMutex();
    if (fila_respostas == nullptr || fila_operacoes == nullptr ||
        mutex_solicitacao == nullptr ||
        configuracao::criar_tarefa(tarefa_operacoes_mestre, "acoes_mestre", nullptr, &tarefa_operacoes) != pdPASS) {
        if (fila_respostas != nullptr) vQueueDelete(fila_respostas);
        if (fila_operacoes != nullptr) vQueueDelete(fila_operacoes);
        if (mutex_solicitacao != nullptr) vSemaphoreDelete(mutex_solicitacao);
        fila_respostas = nullptr;
        fila_operacoes = nullptr;
        mutex_solicitacao = nullptr;
        tarefa_operacoes = nullptr;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(ETIQUETA,
             "Canal bidirecional pronto: consultas gerais e ações autorizadas da equipe");
    return ESP_OK;
}

esp_err_t servico_comandos_encerrar() {
    if (mutex_solicitacao != nullptr &&
        xSemaphoreTake(mutex_solicitacao, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (fila_respostas != nullptr) {
        vQueueDelete(fila_respostas);
        fila_respostas = nullptr;
    }
    if (tarefa_operacoes != nullptr) {
        vTaskDelete(tarefa_operacoes);
        tarefa_operacoes = nullptr;
    }
    if (fila_operacoes != nullptr) {
        vQueueDelete(fila_operacoes);
        fila_operacoes = nullptr;
    }
    if (mutex_solicitacao != nullptr) {
        xSemaphoreGive(mutex_solicitacao);
        vSemaphoreDelete(mutex_solicitacao);
        mutex_solicitacao = nullptr;
    }
    return ESP_OK;
}

void servico_comandos_processar_quadro(DestinoUart origem,
                                       const uint8_t* quadro, size_t tamanho) {
    if (quadro == nullptr || !protocolo_comandos_validar_quadro(quadro, tamanho)) {
        portENTER_CRITICAL(&trava_estatisticas);
        estatisticas.quadros_invalidos++;
        portEXIT_CRITICAL(&trava_estatisticas);
        return;
    }
    cabecalho_quadro_comando_t cabecalho{};
    std::memcpy(&cabecalho, quadro, sizeof(cabecalho));
    const uint8_t origem_esperada = no_para_destino(origem);
    if (cabecalho.origem != origem_esperada ||
        cabecalho.destino != NO_COMANDO_MESTRE) {
        portENTER_CRITICAL(&trava_estatisticas);
        estatisticas.quadros_invalidos++;
        portEXIT_CRITICAL(&trava_estatisticas);
        ESP_LOGW(ETIQUETA, "Comando com identidade incompatível descartado");
        return;
    }
    const uint8_t* carga = quadro + sizeof(cabecalho);
    if (cabecalho.tipo == TIPO_QUADRO_COMANDO_SOLICITACAO) {
        processar_solicitacao(origem, cabecalho, carga);
        return;
    }
    RespostaPendente pendente{};
    pendente.origem = origem;
    pendente.cabecalho = cabecalho;
    if (cabecalho.tamanho_carga != 0)
        std::memcpy(pendente.carga, carga, cabecalho.tamanho_carga);
    if (fila_respostas == nullptr ||
        xQueueSend(fila_respostas, &pendente, 0) != pdTRUE) {
        registrar_erro(ESP_ERR_TIMEOUT);
    } else {
        portENTER_CRITICAL(&trava_estatisticas);
        estatisticas.respostas_recebidas++;
        portEXIT_CRITICAL(&trava_estatisticas);
    }
}

esp_err_t servico_comandos_solicitar(
    DestinoUart destino, codigo_comando_t comando, const void* carga,
    uint16_t tamanho_carga, void* resposta, size_t capacidade_resposta,
    uint16_t* tamanho_resposta, codigo_resposta_comando_t* resultado,
    TickType_t tempo_limite) {
    if (fila_respostas == nullptr || mutex_solicitacao == nullptr ||
        (destino != DestinoUart::Equipe && destino != DestinoUart::Visitantes) ||
        comando == 0 || tempo_limite == 0 ||
        tamanho_carga > COMANDO_TAMANHO_MAXIMO_CARGA ||
        (tamanho_carga != 0 && carga == nullptr) || tamanho_resposta == nullptr ||
        resultado == nullptr) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(mutex_solicitacao, tempo_limite) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    *tamanho_resposta = 0;
    *resultado = RESPOSTA_COMANDO_TIMEOUT;
    RespostaPendente descartada{};
    while (xQueueReceive(fila_respostas, &descartada, 0) == pdTRUE) {}
    uint32_t solicitacao = esp_random();
    if (solicitacao == 0) solicitacao = 1;
    uint8_t quadro[COMANDO_TAMANHO_MAXIMO_QUADRO];
    const size_t tamanho = protocolo_comandos_montar_quadro(
        quadro, sizeof(quadro), TIPO_QUADRO_COMANDO_SOLICITACAO,
        NO_COMANDO_MESTRE, no_para_destino(destino), comando, solicitacao,
        RESPOSTA_COMANDO_OK, carga, tamanho_carga);
    if (tamanho == 0) {
        registrar_erro(ESP_ERR_INVALID_SIZE);
        xSemaphoreGive(mutex_solicitacao);
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t erro = ESP_ERR_TIMEOUT;
    for (unsigned tentativa = 1; tentativa <= configuracao::MAXIMO_TENTATIVAS_COMANDO_UART; ++tentativa) {
        erro = gerenciador_uart_enfileirar(destino, quadro, tamanho, tempo_limite);
        if (erro != ESP_OK) continue;
        erro = ESP_ERR_TIMEOUT;  // Enfileirado; ainda falta a resposta do satélite.
        portENTER_CRITICAL(&trava_estatisticas);
        estatisticas.solicitacoes_enviadas++;
        if (tentativa > 1) estatisticas.repeticoes++;
        portEXIT_CRITICAL(&trava_estatisticas);
        RespostaPendente recebida{};
        const TickType_t inicio = xTaskGetTickCount();
        while (true) {
            const TickType_t decorrido = xTaskGetTickCount() - inicio;
            if (decorrido >= tempo_limite) break;
            if (xQueueReceive(fila_respostas, &recebida,
                              tempo_limite - decorrido) != pdTRUE) break;
            if (recebida.origem != destino ||
                recebida.cabecalho.solicitacao != solicitacao ||
                recebida.cabecalho.comando != comando) continue;
            *resultado = static_cast<codigo_resposta_comando_t>(
                recebida.cabecalho.resultado);
            *tamanho_resposta = recebida.cabecalho.tamanho_carga;
            if (*tamanho_resposta > capacidade_resposta ||
                (*tamanho_resposta != 0 && resposta == nullptr)) {
                erro = ESP_ERR_INVALID_SIZE;
            } else {
                if (*tamanho_resposta != 0)
                    std::memcpy(resposta, recebida.carga, *tamanho_resposta);
                erro = ESP_OK;
            }
            xSemaphoreGive(mutex_solicitacao);
            registrar_erro(erro);
            return erro;
        }
    }
    portENTER_CRITICAL(&trava_estatisticas);
    estatisticas.timeouts++;
    estatisticas.ultimo_erro = erro;
    portEXIT_CRITICAL(&trava_estatisticas);
    xSemaphoreGive(mutex_solicitacao);
    ESP_LOGW(ETIQUETA, "Sem resposta de %s ao comando %u após %u tentativas",
             destino == DestinoUart::Equipe ? "equipe" : "visitantes",
             static_cast<unsigned>(comando), configuracao::MAXIMO_TENTATIVAS_COMANDO_UART);
    return erro;
}

EstatisticasServicoComandos servico_comandos_obter_estatisticas() {
    portENTER_CRITICAL(&trava_estatisticas);
    const EstatisticasServicoComandos copia = estatisticas;
    portEXIT_CRITICAL(&trava_estatisticas);
    return copia;
}
