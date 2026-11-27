// Implementa o atendimento de comandos binários recebidos pelo satélite.
#include "servicos/comandos_satelite.h"
#include "gerenciadores/gerenciador_wifi.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define CAPACIDADES_BASICAS \
    (CAPACIDADE_COMANDO_PING | CAPACIDADE_COMANDO_VERSAO)

static const char *TAG = "comandos_satelite";
static configuracao_servico_comandos_satelite_t configuracao_atual;
static portMUX_TYPE trava = portMUX_INITIALIZER_UNLOCKED;
static uint32_t ultima_solicitacao_recebida;
static uint16_t ultimo_comando_recebido;
static size_t tamanho_ultima_resposta;
static uint8_t ultima_resposta[COMANDO_TAMANHO_MAXIMO_QUADRO];

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
    const cabecalho_quadro_comando_t *cabecalho, const uint8_t *carga_solicitacao,
    uint8_t *resposta,
    uint16_t *tamanho_resposta)
{
    if (cabecalho->comando == COMANDO_OBTER_DIAGNOSTICO_SATELITE) {
        if (cabecalho->tamanho_carga != 0 || configuracao_atual.obter_diagnostico == NULL)
            return RESPOSTA_COMANDO_NAO_SUPORTADO;
        resposta_diagnostico_satelite_t dados = {0};
        if (configuracao_atual.obter_diagnostico(&dados) != ESP_OK)
            return RESPOSTA_COMANDO_ERRO_INTERNO;
        memcpy(resposta, &dados, sizeof(dados));
        *tamanho_resposta = sizeof(dados);
        return RESPOSTA_COMANDO_OK;
    }
    if (cabecalho->comando == COMANDO_OBTER_CLIENTES_WIFI_SATELITE) {
        if (cabecalho->tamanho_carga != 1 || configuracao_atual.obter_clientes_wifi == NULL)
            return RESPOSTA_COMANDO_NAO_SUPORTADO;
        resposta_clientes_wifi_satelite_t dados = {0};
        if (configuracao_atual.obter_clientes_wifi(carga_solicitacao[0], &dados) != ESP_OK)
            return RESPOSTA_COMANDO_ERRO_INTERNO;
        memcpy(resposta, &dados, sizeof(dados));
        *tamanho_resposta = sizeof(dados);
        return RESPOSTA_COMANDO_OK;
    }
    if (cabecalho->comando == COMANDO_GERIR_WIFI_SATELITE) {
        if (cabecalho->tamanho_carga != sizeof(solicitacao_gestao_wifi_satelite_t) ||
            configuracao_atual.gerir_wifi == NULL)
            return RESPOSTA_COMANDO_NAO_SUPORTADO;
        solicitacao_gestao_wifi_satelite_t solicitacao;
        resposta_gestao_wifi_satelite_t dados = {0};
        memcpy(&solicitacao, carga_solicitacao, sizeof(solicitacao));
        if (solicitacao.alvo != configuracao_atual.no_satelite ||
            configuracao_atual.gerir_wifi(&solicitacao, &dados) != ESP_OK)
            return RESPOSTA_COMANDO_ERRO_INTERNO;
        memcpy(resposta, &dados, sizeof(dados));
        *tamanho_resposta = sizeof(dados);
        return RESPOSTA_COMANDO_OK;
    }
    if (cabecalho->comando == COMANDO_AJUSTAR_RADIO_SATELITE) {
        if (cabecalho->tamanho_carga != 0) {
            perfil_radio_satelite_t perfil;
            if (cabecalho->tamanho_carga != sizeof(perfil)) return RESPOSTA_COMANDO_CARGA_INVALIDA;
            memcpy(&perfil, carga_solicitacao, sizeof(perfil));
            const esp_err_t erro = gerenciador_wifi_salvar_perfil(&perfil);
            if (erro == ESP_ERR_INVALID_ARG) return RESPOSTA_COMANDO_CARGA_INVALIDA;
            if (erro != ESP_OK) return RESPOSTA_COMANDO_ERRO_INTERNO;
        }
        resposta_radio_satelite_t dados;
        if (gerenciador_wifi_consultar_perfil(&dados) != ESP_OK)
            return RESPOSTA_COMANDO_ERRO_INTERNO;
        memcpy(resposta, &dados, sizeof(dados));
        *tamanho_resposta = sizeof(dados);
        return RESPOSTA_COMANDO_OK;
    }
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

static void processar_solicitacao(const cabecalho_quadro_comando_t *cabecalho,
                                  const uint8_t *carga_solicitacao)
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
        (void)configuracao_atual.enviar(repetida, tamanho_repetida);
        return;
    }

    uint8_t carga[COMANDO_TAMANHO_MAXIMO_CARGA] = {0};
    uint16_t tamanho_carga = 0;
    const codigo_resposta_comando_t resultado = executar_consulta(
        cabecalho, carga_solicitacao, carga, &tamanho_carga);
    uint8_t quadro[COMANDO_TAMANHO_MAXIMO_QUADRO];
    const size_t tamanho = protocolo_comandos_montar_quadro(
        quadro, sizeof(quadro), TIPO_QUADRO_COMANDO_RESPOSTA,
        configuracao_atual.no_satelite, NO_COMANDO_MESTRE,
        cabecalho->comando, cabecalho->solicitacao, resultado,
        carga, tamanho_carga);
    if (tamanho == 0) return;

    portENTER_CRITICAL(&trava);
    ultima_solicitacao_recebida = cabecalho->solicitacao;
    ultimo_comando_recebido = cabecalho->comando;
    tamanho_ultima_resposta = tamanho;
    memcpy(ultima_resposta, quadro, tamanho);
    portEXIT_CRITICAL(&trava);
    (void)configuracao_atual.enviar(quadro, tamanho);
}

esp_err_t servico_comandos_satelite_iniciar(
    const configuracao_servico_comandos_satelite_t *configuracao)
{
    if (configuracao == NULL || configuracao->nome_satelite == NULL ||
        configuracao->enviar == NULL ||
        (configuracao->no_satelite != NO_COMANDO_EQUIPE &&
         configuracao->no_satelite != NO_COMANDO_VISITANTES))
        return ESP_ERR_INVALID_ARG;
    if (configuracao_atual.enviar != NULL) return ESP_OK;
    configuracao_atual = *configuracao;
    ESP_LOGI(TAG, "%s: canal de comandos do Mestre pronto",
             configuracao_atual.nome_satelite);
    return ESP_OK;
}

void servico_comandos_satelite_processar_quadro(const uint8_t *quadro,
                                               size_t tamanho)
{
    if (configuracao_atual.enviar == NULL || quadro == NULL ||
        !protocolo_comandos_validar_quadro(quadro, tamanho))
        return;

    cabecalho_quadro_comando_t cabecalho;
    memcpy(&cabecalho, quadro, sizeof(cabecalho));
    if (cabecalho.origem != NO_COMANDO_MESTRE ||
        cabecalho.destino != configuracao_atual.no_satelite) {
        ESP_LOGW(TAG, "%s: identidade de comando incompatível",
                 configuracao_atual.nome_satelite);
        return;
    }
    if (cabecalho.tipo == TIPO_QUADRO_COMANDO_SOLICITACAO)
        processar_solicitacao(&cabecalho, quadro + sizeof(cabecalho));
}
