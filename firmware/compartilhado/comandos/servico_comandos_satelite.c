#include "servico_comandos_satelite.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define MAXIMO_TENTATIVAS 3u
#define CAPACIDADES_BASICAS \
    (CAPACIDADE_COMANDO_PING | CAPACIDADE_COMANDO_VERSAO)

typedef struct {
    cabecalho_quadro_comando_t cabecalho;
    uint8_t carga[COMANDO_TAMANHO_MAXIMO_CARGA];
} resposta_pendente_t;

static const char *TAG = "comandos_satelite";
static configuracao_servico_comandos_satelite_t configuracao_atual;
static QueueHandle_t fila_respostas;
static SemaphoreHandle_t mutex_solicitacao;
static portMUX_TYPE trava = portMUX_INITIALIZER_UNLOCKED;
static estatisticas_servico_comandos_satelite_t estatisticas;
static uint32_t ultima_solicitacao_recebida;
static uint16_t ultimo_comando_recebido;
static size_t tamanho_ultima_resposta;
static uint8_t ultima_resposta[COMANDO_TAMANHO_MAXIMO_QUADRO];

static void registrar_erro(esp_err_t erro)
{
    portENTER_CRITICAL(&trava);
    estatisticas.ultimo_erro = erro;
    portEXIT_CRITICAL(&trava);
}

static uint32_t obter_capacidades(void)
{
    return CAPACIDADES_BASICAS | configuracao_atual.capacidades_adicionais;
}

static void copiar_texto(char *destino, size_t capacidade, const char *origem)
{
    if (destino == NULL || capacidade == 0) return;
    const char *texto = origem == NULL ? "" : origem;
    const size_t comprimento = strlen(texto);
    const size_t tamanho = comprimento < capacidade - 1
                               ? comprimento : capacidade - 1;
    memcpy(destino, texto, tamanho);
    destino[tamanho] = '\0';
}

static codigo_resposta_comando_t executar_consulta(
    const cabecalho_quadro_comando_t *cabecalho, uint8_t *resposta,
    uint16_t *tamanho_resposta)
{
    if (cabecalho->tamanho_carga != 0) return RESPOSTA_COMANDO_CARGA_INVALIDA;
    if (cabecalho->comando == COMANDO_PING) {
        const resposta_comando_ping_t dados = {
            .tempo_ativo_ms = (uint32_t)(esp_timer_get_time() / 1000),
        };
        memcpy(resposta, &dados, sizeof(dados));
        *tamanho_resposta = sizeof(dados);
        return RESPOSTA_COMANDO_OK;
    }
    if (cabecalho->comando == COMANDO_OBTER_VERSAO) {
        resposta_comando_versao_t dados = {0};
        copiar_texto(dados.versao, sizeof(dados.versao),
                     esp_app_get_description()->version);
        dados.capacidades = obter_capacidades();
        dados.no = configuracao_atual.no_satelite;
        memcpy(resposta, &dados, sizeof(dados));
        *tamanho_resposta = sizeof(dados);
        return RESPOSTA_COMANDO_OK;
    }
    if (cabecalho->comando == COMANDO_OBTER_CAPACIDADES) {
        const resposta_comando_capacidades_t dados = {
            .capacidades = obter_capacidades(),
        };
        memcpy(resposta, &dados, sizeof(dados));
        *tamanho_resposta = sizeof(dados);
        return RESPOSTA_COMANDO_OK;
    }
    return RESPOSTA_COMANDO_NAO_SUPORTADO;
}

static void processar_solicitacao(const cabecalho_quadro_comando_t *cabecalho)
{
    uint8_t repetida[COMANDO_TAMANHO_MAXIMO_QUADRO];
    size_t tamanho_repetida = 0;
    portENTER_CRITICAL(&trava);
    if (cabecalho->solicitacao == ultima_solicitacao_recebida &&
        cabecalho->comando == ultimo_comando_recebido) {
        tamanho_repetida = tamanho_ultima_resposta;
        if (tamanho_repetida != 0)
            memcpy(repetida, ultima_resposta, tamanho_repetida);
    }
    portEXIT_CRITICAL(&trava);
    if (tamanho_repetida != 0) {
        const esp_err_t erro = configuracao_atual.enviar(repetida, tamanho_repetida);
        portENTER_CRITICAL(&trava);
        estatisticas.repeticoes++;
        if (erro == ESP_OK) {
            estatisticas.respostas_enviadas++;
            estatisticas.ultimo_erro = ESP_OK;
        } else {
            estatisticas.ultimo_erro = erro;
        }
        portEXIT_CRITICAL(&trava);
        return;
    }

    uint8_t carga[COMANDO_TAMANHO_MAXIMO_CARGA] = {0};
    uint16_t tamanho_carga = 0;
    const codigo_resposta_comando_t resultado = executar_consulta(
        cabecalho, carga, &tamanho_carga);
    uint8_t quadro[COMANDO_TAMANHO_MAXIMO_QUADRO];
    const size_t tamanho = protocolo_comandos_montar_quadro(
        quadro, sizeof(quadro), TIPO_QUADRO_COMANDO_RESPOSTA,
        configuracao_atual.no_satelite, NO_COMANDO_MESTRE,
        cabecalho->comando, cabecalho->solicitacao, resultado,
        carga, tamanho_carga);
    if (tamanho != 0) {
        portENTER_CRITICAL(&trava);
        ultima_solicitacao_recebida = cabecalho->solicitacao;
        ultimo_comando_recebido = cabecalho->comando;
        tamanho_ultima_resposta = tamanho;
        memcpy(ultima_resposta, quadro, tamanho);
        portEXIT_CRITICAL(&trava);
    }
    const esp_err_t erro = tamanho == 0 ? ESP_ERR_INVALID_SIZE
                                        : configuracao_atual.enviar(quadro, tamanho);
    portENTER_CRITICAL(&trava);
    estatisticas.solicitacoes_recebidas++;
    if (erro == ESP_OK) {
        estatisticas.respostas_enviadas++;
        estatisticas.ultimo_erro = ESP_OK;
    } else {
        estatisticas.ultimo_erro = erro;
    }
    portEXIT_CRITICAL(&trava);
}

esp_err_t servico_comandos_satelite_iniciar(
    const configuracao_servico_comandos_satelite_t *configuracao)
{
    if (configuracao == NULL || configuracao->nome_satelite == NULL ||
        configuracao->enviar == NULL ||
        (configuracao->no_satelite != NO_COMANDO_EQUIPE &&
         configuracao->no_satelite != NO_COMANDO_VISITANTES))
        return ESP_ERR_INVALID_ARG;
    if (fila_respostas != NULL && mutex_solicitacao != NULL) return ESP_OK;
    configuracao_atual = *configuracao;
    fila_respostas = xQueueCreate(4, sizeof(resposta_pendente_t));
    mutex_solicitacao = xSemaphoreCreateMutex();
    if (fila_respostas == NULL || mutex_solicitacao == NULL) {
        if (fila_respostas != NULL) vQueueDelete(fila_respostas);
        if (mutex_solicitacao != NULL) vSemaphoreDelete(mutex_solicitacao);
        fila_respostas = NULL;
        mutex_solicitacao = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "%s: canal bidirecional de comandos pronto",
             configuracao_atual.nome_satelite);
    return ESP_OK;
}

void servico_comandos_satelite_processar_quadro(const uint8_t *quadro,
                                                  size_t tamanho)
{
    if (fila_respostas == NULL || quadro == NULL ||
        !protocolo_comandos_validar_quadro(quadro, tamanho)) {
        portENTER_CRITICAL(&trava);
        estatisticas.quadros_invalidos++;
        estatisticas.ultimo_erro = ESP_ERR_INVALID_RESPONSE;
        portEXIT_CRITICAL(&trava);
        return;
    }
    cabecalho_quadro_comando_t cabecalho;
    memcpy(&cabecalho, quadro, sizeof(cabecalho));
    if (cabecalho.origem != NO_COMANDO_MESTRE ||
        cabecalho.destino != configuracao_atual.no_satelite) {
        portENTER_CRITICAL(&trava);
        estatisticas.quadros_invalidos++;
        estatisticas.ultimo_erro = ESP_ERR_INVALID_RESPONSE;
        portEXIT_CRITICAL(&trava);
        ESP_LOGW(TAG, "%s: identidade de comando incompatível",
                 configuracao_atual.nome_satelite);
        return;
    }
    if (cabecalho.tipo == TIPO_QUADRO_COMANDO_SOLICITACAO) {
        processar_solicitacao(&cabecalho);
        return;
    }
    resposta_pendente_t pendente = {.cabecalho = cabecalho};
    if (cabecalho.tamanho_carga != 0)
        memcpy(pendente.carga, quadro + sizeof(cabecalho),
               cabecalho.tamanho_carga);
    if (xQueueSend(fila_respostas, &pendente, 0) != pdTRUE) {
        registrar_erro(ESP_ERR_TIMEOUT);
        return;
    }
    portENTER_CRITICAL(&trava);
    estatisticas.respostas_recebidas++;
    estatisticas.ultimo_erro = ESP_OK;
    portEXIT_CRITICAL(&trava);
}

static void atualizar_informacoes_solicitacao(
    informacoes_solicitacao_comando_t *informacoes, uint32_t identificador,
    uint8_t tentativas, int64_t inicio_us)
{
    if (informacoes == NULL) return;
    informacoes->identificador = identificador;
    informacoes->tentativas = tentativas;
    informacoes->duracao_ms = (uint32_t)((esp_timer_get_time() - inicio_us) / 1000);
}

esp_err_t servico_comandos_satelite_solicitar_detalhado(
    codigo_comando_t comando, const void *carga, uint16_t tamanho_carga,
    void *resposta, size_t capacidade_resposta, uint16_t *tamanho_resposta,
    codigo_resposta_comando_t *resultado, TickType_t tempo_limite,
    informacoes_solicitacao_comando_t *informacoes)
{
    if (informacoes != NULL) memset(informacoes, 0, sizeof(*informacoes));
    if (fila_respostas == NULL || mutex_solicitacao == NULL || comando == 0 ||
        tamanho_carga > COMANDO_TAMANHO_MAXIMO_CARGA ||
        (tamanho_carga != 0 && carga == NULL) || tamanho_resposta == NULL ||
        resultado == NULL || tempo_limite == 0)
        return ESP_ERR_INVALID_ARG;
    const int64_t inicio_us = esp_timer_get_time();
    if (xSemaphoreTake(mutex_solicitacao, tempo_limite) != pdTRUE) {
        atualizar_informacoes_solicitacao(informacoes, 0, 0, inicio_us);
        return ESP_ERR_TIMEOUT;
    }

    resposta_pendente_t pendente;
    while (xQueueReceive(fila_respostas, &pendente, 0) == pdTRUE) {}
    *tamanho_resposta = 0;
    *resultado = RESPOSTA_COMANDO_TIMEOUT;
    uint32_t solicitacao = esp_random();
    if (solicitacao == 0) solicitacao = 1;
    uint8_t quadro[COMANDO_TAMANHO_MAXIMO_QUADRO];
    const size_t tamanho = protocolo_comandos_montar_quadro(
        quadro, sizeof(quadro), TIPO_QUADRO_COMANDO_SOLICITACAO,
        configuracao_atual.no_satelite, NO_COMANDO_MESTRE, comando,
        solicitacao, RESPOSTA_COMANDO_OK, carga, tamanho_carga);
    if (tamanho == 0) {
        registrar_erro(ESP_ERR_INVALID_SIZE);
        xSemaphoreGive(mutex_solicitacao);
        atualizar_informacoes_solicitacao(informacoes, solicitacao, 0,
                                          inicio_us);
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t erro = ESP_ERR_TIMEOUT;

    for (unsigned tentativa = 1; tentativa <= MAXIMO_TENTATIVAS; ++tentativa) {
        atualizar_informacoes_solicitacao(
            informacoes, solicitacao, (uint8_t)tentativa, inicio_us);
        ESP_LOGI(TAG, "%s: enviando comando %u ao mestre (tentativa %u/%u)",
                 configuracao_atual.nome_satelite, (unsigned)comando,
                 tentativa, MAXIMO_TENTATIVAS);
        erro = configuracao_atual.enviar(quadro, tamanho);
        if (erro != ESP_OK) continue;
        portENTER_CRITICAL(&trava);
        estatisticas.solicitacoes_enviadas++;
        if (tentativa > 1) estatisticas.repeticoes++;
        portEXIT_CRITICAL(&trava);
        const TickType_t inicio = xTaskGetTickCount();
        while (true) {
            const TickType_t decorrido = xTaskGetTickCount() - inicio;
            if (decorrido >= tempo_limite) break;
            if (xQueueReceive(fila_respostas, &pendente,
                              tempo_limite - decorrido) != pdTRUE) break;
            if (pendente.cabecalho.solicitacao != solicitacao ||
                pendente.cabecalho.comando != comando) continue;
            *resultado = (codigo_resposta_comando_t)pendente.cabecalho.resultado;
            *tamanho_resposta = pendente.cabecalho.tamanho_carga;
            if (*tamanho_resposta > capacidade_resposta ||
                (*tamanho_resposta != 0 && resposta == NULL)) {
                erro = ESP_ERR_INVALID_SIZE;
            } else {
                if (*tamanho_resposta != 0)
                    memcpy(resposta, pendente.carga, *tamanho_resposta);
                erro = ESP_OK;
            }
            registrar_erro(erro);
            xSemaphoreGive(mutex_solicitacao);
            atualizar_informacoes_solicitacao(
                informacoes, solicitacao, (uint8_t)tentativa, inicio_us);
            ESP_LOGI(TAG,
                     "%s: resposta do mestre ao comando %u recebida; resultado=%u",
                     configuracao_atual.nome_satelite, (unsigned)comando,
                     (unsigned)*resultado);
            return erro;
        }
    }
    portENTER_CRITICAL(&trava);
    estatisticas.timeouts++;
    estatisticas.ultimo_erro = erro;
    portEXIT_CRITICAL(&trava);
    xSemaphoreGive(mutex_solicitacao);
    atualizar_informacoes_solicitacao(
        informacoes, solicitacao, MAXIMO_TENTATIVAS, inicio_us);
    ESP_LOGW(TAG, "%s: mestre não respondeu ao comando %u após %u tentativas",
             configuracao_atual.nome_satelite, (unsigned)comando,
             MAXIMO_TENTATIVAS);
    return erro;
}

esp_err_t servico_comandos_satelite_solicitar(
    codigo_comando_t comando, const void *carga, uint16_t tamanho_carga,
    void *resposta, size_t capacidade_resposta, uint16_t *tamanho_resposta,
    codigo_resposta_comando_t *resultado, TickType_t tempo_limite)
{
    return servico_comandos_satelite_solicitar_detalhado(
        comando, carga, tamanho_carga, resposta, capacidade_resposta,
        tamanho_resposta, resultado, tempo_limite, NULL);
}

estatisticas_servico_comandos_satelite_t
servico_comandos_satelite_obter_estatisticas(void)
{
    portENTER_CRITICAL(&trava);
    const estatisticas_servico_comandos_satelite_t copia = estatisticas;
    portEXIT_CRITICAL(&trava);
    return copia;
}
