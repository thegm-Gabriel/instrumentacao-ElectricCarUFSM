#include "servico_ota_satelite.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "protocolo_ota_satelites.h"

static const char *TAG = "ota_satelite";
static configuracao_servico_ota_satelite_t configuracao_atual;
static situacao_servico_ota_satelite_t situacao;
static portMUX_TYPE trava = portMUX_INITIALIZER_UNLOCKED;
static const esp_partition_t *particao_destino;
static esp_ota_handle_t manipulador_ota;
static uint8_t sha256_esperado[OTA_SATELITE_TAMANHO_SHA256];
static uint32_t proxima_sequencia;
static int64_t ultimo_dado_ms;

static void copiar_situacao(situacao_servico_ota_satelite_t *destino)
{
    portENTER_CRITICAL(&trava);
    *destino = situacao;
    portEXIT_CRITICAL(&trava);
}

static void registrar_erro(esp_err_t erro)
{
    portENTER_CRITICAL(&trava);
    situacao.ultimo_erro = erro;
    if (erro != ESP_OK) situacao.falhas++;
    portEXIT_CRITICAL(&trava);
}

static void limpar_transferencia(void)
{
    particao_destino = NULL;
    manipulador_ota = 0;
    memset(sha256_esperado, 0, sizeof(sha256_esperado));
    proxima_sequencia = 0;
    ultimo_dado_ms = 0;
    portENTER_CRITICAL(&trava);
    situacao.recebendo = false;
    situacao.sessao = 0;
    situacao.tamanho_total = 0;
    situacao.bytes_recebidos = 0;
    situacao.quadros_recebidos = 0;
    situacao.versao_destino[0] = '\0';
    portEXIT_CRITICAL(&trava);
}

static void abortar_transferencia(esp_err_t motivo, const char *mensagem)
{
    if (manipulador_ota != 0) (void)esp_ota_abort(manipulador_ota);
    ESP_LOGE(TAG, "%s: %s", mensagem, esp_err_to_name(motivo));
    registrar_erro(motivo);
    limpar_transferencia();
}

static esp_err_t responder(const cabecalho_quadro_ota_satelite_t *recebido,
                           codigo_resposta_ota_satelite_t codigo,
                           fase_ota_satelite_t fase, esp_err_t erro,
                           uint32_t proximo_deslocamento)
{
    if (configuracao_atual.enviar == NULL || recebido == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const carga_resposta_ota_satelite_t resposta = {
        .codigo = (uint8_t)codigo,
        .fase = (uint8_t)fase,
        .erro_esp = (int32_t)erro,
    };
    uint8_t quadro[OTA_SATELITE_TAMANHO_CABECALHO +
                   sizeof(carga_resposta_ota_satelite_t) +
                   OTA_SATELITE_TAMANHO_CRC];
    const size_t tamanho = protocolo_ota_satelites_montar_quadro(
        quadro, sizeof(quadro), OTA_SATELITE_TIPO_RESPOSTA,
        recebido->sessao, recebido->sequencia, proximo_deslocamento,
        &resposta, sizeof(resposta));
    return tamanho == 0 ? ESP_ERR_INVALID_SIZE
                        : configuracao_atual.enviar(quadro, tamanho);
}

static esp_err_t iniciar_transferencia(
    const cabecalho_quadro_ota_satelite_t *cabecalho, const uint8_t *carga)
{
    if (cabecalho->tamanho_carga != sizeof(carga_inicio_ota_satelite_t) ||
        cabecalho->sequencia != 0 || cabecalho->deslocamento != 0) {
        (void)responder(cabecalho, OTA_SATELITE_RESPOSTA_TAMANHO_INVALIDO,
                        OTA_SATELITE_FASE_FALHA, ESP_ERR_INVALID_SIZE, 0);
        return ESP_ERR_INVALID_SIZE;
    }
    carga_inicio_ota_satelite_t inicio;
    memcpy(&inicio, carga, sizeof(inicio));
    if (inicio.tamanho_firmware == 0 ||
        memchr(inicio.versao, '\0', sizeof(inicio.versao)) == NULL) {
        (void)responder(cabecalho, OTA_SATELITE_RESPOSTA_TAMANHO_INVALIDO,
                        OTA_SATELITE_FASE_FALHA, ESP_ERR_INVALID_ARG, 0);
        return ESP_ERR_INVALID_ARG;
    }

    situacao_servico_ota_satelite_t atual;
    copiar_situacao(&atual);
    if (atual.recebendo && atual.sessao == cabecalho->sessao &&
        atual.bytes_recebidos == 0 &&
        atual.tamanho_total == inicio.tamanho_firmware) {
        return responder(cabecalho, OTA_SATELITE_RESPOSTA_OK,
                         OTA_SATELITE_FASE_RECEBENDO, ESP_OK, 0);
    }

    if (manipulador_ota != 0) (void)esp_ota_abort(manipulador_ota);
    limpar_transferencia();
    particao_destino = esp_ota_get_next_update_partition(NULL);
    if (particao_destino == NULL || inicio.tamanho_firmware > particao_destino->size) {
        particao_destino = NULL;
        (void)responder(cabecalho, OTA_SATELITE_RESPOSTA_TAMANHO_INVALIDO,
                        OTA_SATELITE_FASE_FALHA, ESP_ERR_INVALID_SIZE, 0);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t erro = esp_ota_begin(particao_destino, inicio.tamanho_firmware,
                                   &manipulador_ota);
    if (erro != ESP_OK) {
        particao_destino = NULL;
        (void)responder(cabecalho, OTA_SATELITE_RESPOSTA_GRAVACAO_FALHOU,
                        OTA_SATELITE_FASE_FALHA, erro, 0);
        registrar_erro(erro);
        return erro;
    }

    memcpy(sha256_esperado, inicio.sha256, sizeof(sha256_esperado));
    proxima_sequencia = 1;
    ultimo_dado_ms = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&trava);
    situacao.recebendo = true;
    situacao.sessao = cabecalho->sessao;
    situacao.tamanho_total = inicio.tamanho_firmware;
    situacao.bytes_recebidos = 0;
    situacao.quadros_recebidos = 0;
    memcpy(situacao.versao_destino, inicio.versao, sizeof(situacao.versao_destino));
    situacao.versao_destino[sizeof(situacao.versao_destino) - 1] = '\0';
    situacao.ultimo_erro = ESP_OK;
    portEXIT_CRITICAL(&trava);
    ESP_LOGW(TAG, "%s: recebendo firmware %s (%lu bytes) pela UART",
             configuracao_atual.nome_satelite, situacao.versao_destino,
             (unsigned long)inicio.tamanho_firmware);
    return responder(cabecalho, OTA_SATELITE_RESPOSTA_OK,
                     OTA_SATELITE_FASE_RECEBENDO, ESP_OK, 0);
}

static esp_err_t gravar_dados(const cabecalho_quadro_ota_satelite_t *cabecalho,
                              const uint8_t *carga)
{
    situacao_servico_ota_satelite_t atual;
    copiar_situacao(&atual);
    if (!atual.recebendo || cabecalho->sessao != atual.sessao) {
        (void)responder(cabecalho, OTA_SATELITE_RESPOSTA_SESSAO_INVALIDA,
                        OTA_SATELITE_FASE_FALHA, ESP_ERR_INVALID_STATE,
                        atual.bytes_recebidos);
        return ESP_ERR_INVALID_STATE;
    }
    if (cabecalho->sequencia + 1 == proxima_sequencia &&
        cabecalho->deslocamento + cabecalho->tamanho_carga == atual.bytes_recebidos) {
        return responder(cabecalho, OTA_SATELITE_RESPOSTA_OK,
                         OTA_SATELITE_FASE_RECEBENDO, ESP_OK,
                         atual.bytes_recebidos);
    }
    if (cabecalho->sequencia != proxima_sequencia) {
        (void)responder(cabecalho, OTA_SATELITE_RESPOSTA_SEQUENCIA_INVALIDA,
                        OTA_SATELITE_FASE_RECEBENDO, ESP_ERR_INVALID_STATE,
                        atual.bytes_recebidos);
        return ESP_ERR_INVALID_STATE;
    }
    if (cabecalho->deslocamento != atual.bytes_recebidos ||
        cabecalho->tamanho_carga == 0 ||
        atual.bytes_recebidos + cabecalho->tamanho_carga > atual.tamanho_total) {
        (void)responder(cabecalho, OTA_SATELITE_RESPOSTA_DESLOCAMENTO_INVALIDO,
                        OTA_SATELITE_FASE_RECEBENDO, ESP_ERR_INVALID_SIZE,
                        atual.bytes_recebidos);
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_err_t erro = esp_ota_write(manipulador_ota, carga,
                                         cabecalho->tamanho_carga);
    if (erro != ESP_OK) {
        (void)responder(cabecalho, OTA_SATELITE_RESPOSTA_GRAVACAO_FALHOU,
                        OTA_SATELITE_FASE_FALHA, erro, atual.bytes_recebidos);
        abortar_transferencia(erro, "Falha ao gravar o bloco OTA");
        return erro;
    }
    proxima_sequencia++;
    ultimo_dado_ms = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&trava);
    situacao.bytes_recebidos += cabecalho->tamanho_carga;
    situacao.quadros_recebidos++;
    situacao.ultimo_erro = ESP_OK;
    const uint32_t proximo = situacao.bytes_recebidos;
    portEXIT_CRITICAL(&trava);
    return responder(cabecalho, OTA_SATELITE_RESPOSTA_OK,
                     OTA_SATELITE_FASE_RECEBENDO, ESP_OK, proximo);
}

static esp_err_t finalizar_transferencia(
    const cabecalho_quadro_ota_satelite_t *cabecalho)
{
    situacao_servico_ota_satelite_t atual;
    copiar_situacao(&atual);
    if (!atual.recebendo || cabecalho->sessao != atual.sessao ||
        cabecalho->sequencia != proxima_sequencia ||
        cabecalho->tamanho_carga != 0 || atual.bytes_recebidos != atual.tamanho_total) {
        (void)responder(cabecalho, OTA_SATELITE_RESPOSTA_ESTADO_INVALIDO,
                        OTA_SATELITE_FASE_FALHA, ESP_ERR_INVALID_STATE,
                        atual.bytes_recebidos);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t erro = esp_ota_end(manipulador_ota);
    manipulador_ota = 0;
    if (erro == ESP_OK) {
        uint8_t sha256_obtido[OTA_SATELITE_TAMANHO_SHA256];
        erro = esp_partition_get_sha256(particao_destino, sha256_obtido);
        if (erro == ESP_OK &&
            memcmp(sha256_obtido, sha256_esperado, sizeof(sha256_obtido)) != 0) {
            erro = ESP_ERR_INVALID_CRC;
        }
    }
    if (erro == ESP_OK) {
        esp_app_desc_t descricao;
        erro = esp_ota_get_partition_description(particao_destino, &descricao);
        if (erro == ESP_OK &&
            strcmp(descricao.version, atual.versao_destino) != 0) {
            ESP_LOGE(TAG, "Versão da imagem (%s) difere do manifesto (%s)",
                     descricao.version, atual.versao_destino);
            erro = ESP_ERR_INVALID_VERSION;
        }
    }
    if (erro == ESP_OK) erro = esp_ota_set_boot_partition(particao_destino);
    if (erro != ESP_OK) {
        (void)responder(cabecalho, OTA_SATELITE_RESPOSTA_INTEGRIDADE_FALHOU,
                        OTA_SATELITE_FASE_FALHA, erro, atual.bytes_recebidos);
        abortar_transferencia(erro, "Imagem OTA rejeitada");
        return erro;
    }

    ESP_LOGW(TAG, "%s: firmware %s validado; reiniciando em 1 segundo",
             configuracao_atual.nome_satelite, atual.versao_destino);
    erro = responder(cabecalho, OTA_SATELITE_RESPOSTA_OK,
                     OTA_SATELITE_FASE_PRONTO_PARA_REINICIAR, ESP_OK,
                     atual.bytes_recebidos);
    portENTER_CRITICAL(&trava);
    situacao.recebendo = false;
    situacao.ultimo_erro = erro;
    portEXIT_CRITICAL(&trava);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

esp_err_t servico_ota_satelite_iniciar(
    const configuracao_servico_ota_satelite_t *configuracao)
{
    if (configuracao == NULL || configuracao->nome_satelite == NULL ||
        configuracao->enviar == NULL || configuracao->tempo_limite_sem_dados_ms < 1000) {
        return ESP_ERR_INVALID_ARG;
    }
    configuracao_atual = *configuracao;
    limpar_transferencia();
    portENTER_CRITICAL(&trava);
    situacao = (situacao_servico_ota_satelite_t){0};
    situacao.iniciado = true;
    situacao.ultimo_erro = ESP_OK;
    portEXIT_CRITICAL(&trava);
    ESP_LOGI(TAG, "%s: receptor OTA pela UART pronto; firmware atual=%s",
             configuracao_atual.nome_satelite, esp_app_get_description()->version);
    return ESP_OK;
}

esp_err_t servico_ota_satelite_confirmar_firmware_em_execucao(void)
{
    esp_err_t erro = ESP_OK;
    const esp_partition_t *execucao = esp_ota_get_running_partition();
    esp_ota_img_states_t estado_imagem = ESP_OTA_IMG_UNDEFINED;
    if (execucao != NULL &&
        esp_ota_get_state_partition(execucao, &estado_imagem) == ESP_OK &&
        estado_imagem == ESP_OTA_IMG_PENDING_VERIFY) {
        erro = esp_ota_mark_app_valid_cancel_rollback();
    }
    if (erro != ESP_OK) {
        registrar_erro(erro);
        ESP_LOGE(TAG, "Não foi possível confirmar o firmware em execução: %s",
                 esp_err_to_name(erro));
        return erro;
    }
    return ESP_OK;
}

esp_err_t servico_ota_satelite_processar_quadro(const uint8_t *quadro,
                                                size_t tamanho)
{
    if (!situacao.iniciado) return ESP_ERR_INVALID_STATE;
    if (!protocolo_ota_satelites_validar_quadro(quadro, tamanho)) {
        registrar_erro(ESP_ERR_INVALID_CRC);
        return ESP_ERR_INVALID_CRC;
    }
    cabecalho_quadro_ota_satelite_t cabecalho;
    memcpy(&cabecalho, quadro, sizeof(cabecalho));
    const uint8_t *carga = quadro + sizeof(cabecalho);
    switch (cabecalho.tipo) {
        case OTA_SATELITE_TIPO_INICIAR:
            return iniciar_transferencia(&cabecalho, carga);
        case OTA_SATELITE_TIPO_DADOS:
            return gravar_dados(&cabecalho, carga);
        case OTA_SATELITE_TIPO_FINALIZAR:
            return finalizar_transferencia(&cabecalho);
        case OTA_SATELITE_TIPO_CANCELAR:
            if (manipulador_ota != 0) (void)esp_ota_abort(manipulador_ota);
            (void)responder(&cabecalho, OTA_SATELITE_RESPOSTA_OK,
                            OTA_SATELITE_FASE_CANCELADO, ESP_OK, 0);
            limpar_transferencia();
            ESP_LOGW(TAG, "%s: transferência OTA cancelada pelo mestre",
                     configuracao_atual.nome_satelite);
            return ESP_OK;
        case OTA_SATELITE_TIPO_RESPOSTA:
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

void servico_ota_satelite_verificar_timeout(void)
{
    situacao_servico_ota_satelite_t atual;
    copiar_situacao(&atual);
    if (!atual.recebendo || ultimo_dado_ms == 0) return;
    const int64_t agora_ms = esp_timer_get_time() / 1000;
    if ((uint64_t)(agora_ms - ultimo_dado_ms) <=
        configuracao_atual.tempo_limite_sem_dados_ms) return;
    abortar_transferencia(ESP_ERR_TIMEOUT,
                          "Transferência OTA interrompida por inatividade");
}

situacao_servico_ota_satelite_t servico_ota_satelite_obter_situacao(void)
{
    situacao_servico_ota_satelite_t copia;
    copiar_situacao(&copia);
    return copia;
}
