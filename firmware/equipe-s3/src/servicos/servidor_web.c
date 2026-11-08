// Implementa as rotas HTTP e publica a telemetria recebida.
#include "servicos/servidor_web.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nucleo/configuracao.h"
#include "servicos/diagnostico_sistema.h"
#include "servicos/autenticacao_comandos.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gerenciadores/gerenciador_wifi.h"
#include "gerenciadores/gerenciador_uart.h"
#include "servicos/painel_web.h"
#include "protocolo_comandos.h"
#include "servicos/receptor_uart.h"
#include "servico_comandos_satelite.h"
#include "servico_ota_satelite.h"

static const char *TAG = "servidor_web";
// Cada chamada usa uma tarefa de 8 KiB; uma vaga exclusiva preserva as acoes.
static SemaphoreHandle_t vagas_api_mestre_consultas;
static SemaphoreHandle_t vaga_api_mestre_acao;
#define MAXIMO_CONSULTAS_MESTRE_SIMULTANEAS 2

static esp_err_t responder_clientes_wifi(httpd_req_t *requisicao)
{
    situacao_clientes_wifi_t situacao;
    const esp_err_t erro = gerenciador_wifi_obter_clientes(&situacao);
    if (erro != ESP_OK) return httpd_resp_send_err(requisicao,
        HTTPD_500_INTERNAL_SERVER_ERROR, "lista Wi-Fi indisponivel");
    char json[1536];
    int usados = snprintf(json, sizeof(json),
        "{\"ok\":true,\"limite_sessao_minutos\":%u,\"quantidade\":%u,\"clientes\":[",
        (unsigned)situacao.limite_sessao_minutos, (unsigned)situacao.quantidade);
    for (unsigned i = 0; i < situacao.quantidade && usados > 0 && usados < (int)sizeof(json); ++i) {
        const cliente_wifi_ap_t *cliente = &situacao.clientes[i];
        usados += snprintf(json + usados, sizeof(json) - (size_t)usados,
            "%s{\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"aid\":%u,\"conectado_ms\":%lu}",
            i ? "," : "", cliente->mac[0], cliente->mac[1], cliente->mac[2],
            cliente->mac[3], cliente->mac[4], cliente->mac[5],
            (unsigned)cliente->aid, (unsigned long)cliente->conectado_ms);
    }
    if (usados < 0 || usados >= (int)sizeof(json) - 3)
        return httpd_resp_send_err(requisicao, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "lista Wi-Fi excedeu buffer");
    usados += snprintf(json + usados, sizeof(json) - (size_t)usados, "]}");
    httpd_resp_set_hdr(requisicao, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_type(requisicao, "application/json");
    return httpd_resp_send(requisicao, json, usados);
}

static esp_err_t ler_corpo_wifi(httpd_req_t *requisicao, char corpo[128])
{
    if (requisicao->content_len <= 0 || requisicao->content_len >= 128)
        return ESP_ERR_INVALID_SIZE;
    int recebidos = 0;
    while (recebidos < requisicao->content_len) {
        const int n = httpd_req_recv(requisicao, corpo + recebidos,
                                    requisicao->content_len - recebidos);
        if (n <= 0) return ESP_FAIL;
        recebidos += n;
    }
    corpo[recebidos] = '\0';
    return ESP_OK;
}

static esp_err_t responder_limite_wifi(httpd_req_t *requisicao)
{
    if (!autenticacao_comandos_autorizar(requisicao)) return ESP_OK;
    char corpo[128];
    if (ler_corpo_wifi(requisicao, corpo) != ESP_OK)
        return httpd_resp_send_err(requisicao, HTTPD_400_BAD_REQUEST, "corpo invalido");
    const char *campo = strstr(corpo, "\"minutos\"");
    if (!campo || !(campo = strchr(campo, ':')))
        return httpd_resp_send_err(requisicao, HTTPD_400_BAD_REQUEST, "minutos ausentes");
    ++campo;
    while (*campo == ' ' || *campo == '\t') ++campo;
    char *fim = NULL;
    const unsigned long minutos = strtoul(campo, &fim, 10);
    if (fim == campo || minutos > 1440 || (*fim != '}' && *fim != ',' && *fim != ' '))
        return httpd_resp_send_err(requisicao, HTTPD_400_BAD_REQUEST, "minutos invalidos");
    const esp_err_t erro = gerenciador_wifi_definir_limite_sessao((uint16_t)minutos);
    if (erro != ESP_OK) return httpd_resp_send_err(requisicao,
        HTTPD_500_INTERNAL_SERVER_ERROR, "nao foi possivel salvar limite");
    char json[72];
    const int tamanho = snprintf(json, sizeof(json),
        "{\"ok\":true,\"limite_sessao_minutos\":%lu}", minutos);
    httpd_resp_set_type(requisicao, "application/json");
    return httpd_resp_send(requisicao, json, tamanho);
}

static esp_err_t responder_desconexao_wifi(httpd_req_t *requisicao)
{
    if (!autenticacao_comandos_autorizar(requisicao)) return ESP_OK;
    char corpo[128];
    if (ler_corpo_wifi(requisicao, corpo) != ESP_OK)
        return httpd_resp_send_err(requisicao, HTTPD_400_BAD_REQUEST, "corpo invalido");
    const char *campo = strstr(corpo, "\"mac\"");
    if (!campo || !(campo = strchr(campo, ':')) || !(campo = strchr(campo, '"')))
        return httpd_resp_send_err(requisicao, HTTPD_400_BAD_REQUEST, "MAC ausente");
    unsigned partes[6];
    ++campo;
    if (strlen(campo) < 18 || campo[2] != ':' || campo[5] != ':' ||
        campo[8] != ':' || campo[11] != ':' || campo[14] != ':' ||
        campo[17] != '"' ||
        sscanf(campo, "%2x:%2x:%2x:%2x:%2x:%2x",
               &partes[0], &partes[1], &partes[2],
               &partes[3], &partes[4], &partes[5]) != 6)
        return httpd_resp_send_err(requisicao, HTTPD_400_BAD_REQUEST, "MAC invalido");
    uint8_t mac[6];
    for (unsigned i = 0; i < 6; ++i) mac[i] = (uint8_t)partes[i];
    const esp_err_t erro = gerenciador_wifi_desconectar_cliente(mac);
    if (erro != ESP_OK) return httpd_resp_send_err(requisicao,
        HTTPD_404_NOT_FOUND, "cliente nao esta conectado");
    httpd_resp_set_type(requisicao, "application/json");
    return httpd_resp_send(requisicao, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t responder_acesso_comandos(httpd_req_t *requisicao)
{
    if (!autenticacao_comandos_autorizar(requisicao)) return ESP_OK;
    httpd_resp_set_hdr(requisicao, "Cache-Control", "no-store");
    httpd_resp_set_type(requisicao, "application/json");
    return httpd_resp_send(requisicao, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}


static esp_err_t responder_painel(httpd_req_t *requisicao)
{
    ESP_LOGI(TAG, "Recebido GET / do cliente");
    httpd_resp_set_hdr(requisicao, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_type(requisicao, "text/html; charset=utf-8");
    const esp_err_t erro = httpd_resp_send(
        requisicao, PAGINA_WEB, HTTPD_RESP_USE_STRLEN);
    if (erro != ESP_OK) {
        ESP_LOGW(TAG, "Falha ao enviar a página principal: %s",
                 esp_err_to_name(erro));
    }
    return erro;
}

static esp_err_t responder_painel_equipe(httpd_req_t *requisicao)
{
    httpd_resp_set_hdr(requisicao, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_type(requisicao, "text/html; charset=utf-8");
    return httpd_resp_send(requisicao, PAGINA_WEB, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t responder_mestre_pagina(httpd_req_t *requisicao)
{
    httpd_resp_set_hdr(requisicao, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_type(requisicao, "text/html; charset=utf-8");
    return httpd_resp_send(requisicao, PAGINA_WEB,
                           HTTPD_RESP_USE_STRLEN);
}

static void escapar_texto_json(char *destino, size_t capacidade,
                               const char *origem)
{
    if (destino == NULL || capacidade == 0) return;
    size_t saida = 0;
    for (size_t entrada = 0; origem != NULL && origem[entrada] != '\0' &&
                            saida + 1 < capacidade; ++entrada) {
        const unsigned char caractere = (unsigned char)origem[entrada];
        if ((caractere == '"' || caractere == '\\') && saida + 2 < capacidade) {
            destino[saida++] = '\\';
            destino[saida++] = (char)caractere;
        } else if (caractere >= 0x20) {
            destino[saida++] = (char)caractere;
        }
    }
    destino[saida] = '\0';
}

static codigo_comando_t localizar_consulta_mestre(const char *consulta)
{
    if (strcmp(consulta, "controle_remoto") == 0) return COMANDO_OBTER_CONTROLE_REMOTO;
    if (strcmp(consulta, "ping") == 0) return COMANDO_PING;
    if (strcmp(consulta, "versao") == 0) return COMANDO_OBTER_VERSAO;
    if (strcmp(consulta, "saude") == 0) return COMANDO_OBTER_SAUDE;
    if (strcmp(consulta, "telemetria") == 0) return COMANDO_OBTER_TELEMETRIA;
    if (strcmp(consulta, "uart") == 0) return COMANDO_OBTER_ESTADO_UART;
    if (strcmp(consulta, "ota") == 0) return COMANDO_OBTER_ESTADO_OTA;
    if (strcmp(consulta, "wifi") == 0) return COMANDO_OBTER_ESTADO_WIFI;
    if (strcmp(consulta, "diagnostico_mestre") == 0)
        return COMANDO_OBTER_DIAGNOSTICO_MESTRE;
    if (strcmp(consulta, "configuracao") == 0)
        return COMANDO_OBTER_CONFIGURACAO_MESTRE;
    if (strcmp(consulta, "sincronizacao") == 0)
        return COMANDO_OBTER_ESTADO_SINCRONIZACAO;
    if (strcmp(consulta, "simulador") == 0)
        return COMANDO_OBTER_ESTADO_SIMULADOR;
    if (strcmp(consulta, "rede_wifi_salva") == 0)
        return COMANDO_OBTER_REDE_WIFI_SALVA;
    if (strcmp(consulta, "satelite_equipe_ping") == 0 ||
        strcmp(consulta, "satelite_equipe_versao") == 0 ||
        strcmp(consulta, "satelite_visitantes_ping") == 0 ||
        strcmp(consulta, "satelite_visitantes_versao") == 0 ||
        strcmp(consulta, "satelite_visitantes_diagnostico") == 0 ||
        strcmp(consulta, "satelite_visitantes_clientes") == 0)
        return COMANDO_CONSULTAR_SATELITE;
    if (strcmp(consulta, "satelite_visitantes_limite") == 0 ||
        strcmp(consulta, "satelite_visitantes_kick") == 0)
        return COMANDO_GERIR_WIFI_SATELITE;
    if (strcmp(consulta, "verificar_ota") == 0)
        return COMANDO_SOLICITAR_VERIFICACAO_OTA;
    if (strcmp(consulta, "autorizar_mestre") == 0)
        return COMANDO_AUTORIZAR_OTA_MESTRE;
    if (strcmp(consulta, "autorizar_equipe") == 0)
        return COMANDO_AUTORIZAR_OTA_EQUIPE;
    if (strcmp(consulta, "autorizar_visitantes") == 0)
        return COMANDO_AUTORIZAR_OTA_VISITANTES;
    if (strcmp(consulta, "autorizar_todos") == 0)
        return COMANDO_AUTORIZAR_OTA_TODOS;
    if (strcmp(consulta, "cancelar_ota") == 0) return COMANDO_CANCELAR_OTA;
    if (strcmp(consulta, "resumo_terminal") == 0)
        return COMANDO_SOLICITAR_RESUMO_TERMINAL;
    if (strcmp(consulta, "definir_marcha") == 0 ||
        strcmp(consulta, "velocidade_maxima") == 0 ||
        strcmp(consulta, "aceleracao_maxima") == 0 ||
        strcmp(consulta, "parada_remota") == 0 ||
        strcmp(consulta, "alternar_relatorios") == 0 ||
        strcmp(consulta, "zerar_percurso") == 0 ||
        strcmp(consulta, "sincronizar_google") == 0 ||
        strcmp(consulta, "intervalo_relatorio") == 0 ||
        strcmp(consulta, "intervalo_ota") == 0 ||
        strcmp(consulta, "intervalo_telemetria_ota") == 0 ||
        strcmp(consulta, "wifi_preparar") == 0 ||
        strcmp(consulta, "wifi_conectar_preparada") == 0 ||
        strcmp(consulta, "wifi_salvar_preparada") == 0 ||
        strcmp(consulta, "wifi_conectar_salva") == 0 ||
        strcmp(consulta, "wifi_excluir_salva") == 0 ||
        strcmp(consulta, "wifi_automatico") == 0 ||
        strcmp(consulta, "wifi_alterar_principal") == 0 ||
        strcmp(consulta, "wifi_restaurar_principal") == 0 ||
        strcmp(consulta, "wifi_politica") == 0 ||
        strcmp(consulta, "simulador_telemetria") == 0) {
        return COMANDO_EXECUTAR_ACAO_MESTRE;
    }
    return 0;
}

static bool comando_exige_confirmacao(codigo_comando_t comando)
{
    switch (comando) {
        case COMANDO_SOLICITAR_VERIFICACAO_OTA:
        case COMANDO_AUTORIZAR_OTA_MESTRE:
        case COMANDO_AUTORIZAR_OTA_EQUIPE:
        case COMANDO_AUTORIZAR_OTA_VISITANTES:
        case COMANDO_AUTORIZAR_OTA_TODOS:
        case COMANDO_CANCELAR_OTA:
        case COMANDO_SOLICITAR_RESUMO_TERMINAL:
        case COMANDO_EXECUTAR_ACAO_MESTRE:
        case COMANDO_GERIR_WIFI_SATELITE:
            return true;
        default:
            return false;
    }
}

static acao_mestre_t localizar_acao_mestre(const char *consulta)
{
    if (strcmp(consulta, "definir_marcha") == 0) return ACAO_MESTRE_DEFINIR_MARCHA;
    if (strcmp(consulta, "velocidade_maxima") == 0) return ACAO_MESTRE_DEFINIR_VELOCIDADE_MAXIMA;
    if (strcmp(consulta, "aceleracao_maxima") == 0) return ACAO_MESTRE_DEFINIR_ACELERACAO_MAXIMA;
    if (strcmp(consulta, "parada_remota") == 0) return ACAO_MESTRE_DEFINIR_PARADA_REMOTA;
    if (strcmp(consulta, "alternar_relatorios") == 0)
        return ACAO_MESTRE_ALTERNAR_RELATORIOS;
    if (strcmp(consulta, "zerar_percurso") == 0)
        return ACAO_MESTRE_ZERAR_PERCURSO;
    if (strcmp(consulta, "sincronizar_google") == 0)
        return ACAO_MESTRE_SINCRONIZAR_GOOGLE;
    if (strcmp(consulta, "intervalo_relatorio") == 0)
        return ACAO_MESTRE_DEFINIR_INTERVALO_RELATORIO;
    if (strcmp(consulta, "intervalo_ota") == 0)
        return ACAO_MESTRE_DEFINIR_INTERVALO_OTA;
    if (strcmp(consulta, "intervalo_telemetria_ota") == 0)
        return ACAO_MESTRE_DEFINIR_INTERVALO_TELEMETRIA_OTA;
    if (strcmp(consulta, "wifi_preparar") == 0)
        return ACAO_MESTRE_PREPARAR_REDE_WIFI;
    if (strcmp(consulta, "wifi_conectar_preparada") == 0)
        return ACAO_MESTRE_CONECTAR_REDE_WIFI_PREPARADA;
    if (strcmp(consulta, "wifi_salvar_preparada") == 0)
        return ACAO_MESTRE_SALVAR_REDE_WIFI_PREPARADA;
    if (strcmp(consulta, "wifi_conectar_salva") == 0)
        return ACAO_MESTRE_CONECTAR_REDE_WIFI_SALVA;
    if (strcmp(consulta, "wifi_excluir_salva") == 0)
        return ACAO_MESTRE_EXCLUIR_REDE_WIFI_SALVA;
    if (strcmp(consulta, "wifi_automatico") == 0)
        return ACAO_MESTRE_DEFINIR_WIFI_AUTOMATICO;
    if (strcmp(consulta, "wifi_alterar_principal") == 0)
        return ACAO_MESTRE_ALTERAR_REDE_WIFI_PRINCIPAL;
    if (strcmp(consulta, "wifi_restaurar_principal") == 0)
        return ACAO_MESTRE_RESTAURAR_REDE_WIFI_PRINCIPAL;
    if (strcmp(consulta, "wifi_politica") == 0)
        return ACAO_MESTRE_DEFINIR_POLITICA_WIFI;
    if (strcmp(consulta, "simulador_telemetria") == 0)
        return ACAO_MESTRE_DEFINIR_SIMULADOR;
    return 0;
}

static bool converter_uint32(const char *texto, uint32_t *valor)
{
    if (texto == NULL || valor == NULL || texto[0] == '\0') return false;
    char *fim = NULL;
    const unsigned long numero = strtoul(texto, &fim, 10);
    if (fim == texto || *fim != '\0' || numero > UINT32_MAX) return false;
    *valor = (uint32_t)numero;
    return true;
}

static char *proximo_campo(char **cursor)
{
    if (cursor == NULL || *cursor == NULL) return NULL;
    char *campo = *cursor;
    char *fim = strchr(campo, '\n');
    if (fim == NULL) {
        *cursor = NULL;
    } else {
        *fim = '\0';
        *cursor = fim + 1;
    }
    return campo;
}

static esp_err_t receber_corpo_texto(httpd_req_t *requisicao, char *corpo,
                                     size_t capacidade)
{
    if (capacidade == 0 ||
        (size_t)requisicao->content_len >= capacidade) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t recebido = 0;
    while (recebido < (size_t)requisicao->content_len) {
        const int trecho = httpd_req_recv(
            requisicao, corpo + recebido,
            (size_t)requisicao->content_len - recebido);
        if (trecho <= 0) return ESP_FAIL;
        recebido += (size_t)trecho;
    }
    corpo[recebido] = '\0';
    return ESP_OK;
}

static bool preparar_acao_mestre(const char *consulta, char *corpo,
                                 solicitacao_acao_mestre_t *solicitacao)
{
    *solicitacao = (solicitacao_acao_mestre_t){0};
    solicitacao->acao = (uint8_t)localizar_acao_mestre(consulta);
    if (solicitacao->acao == 0) return false;
    char *cursor = corpo;
    char *campo = NULL;
    uint32_t valor = 0;
    switch ((acao_mestre_t)solicitacao->acao) {
        case ACAO_MESTRE_DEFINIR_INTERVALO_RELATORIO:
            campo = proximo_campo(&cursor);
            if (!converter_uint32(campo, &valor) || valor > 3) return false;
            solicitacao->alvo = (uint8_t)valor;
            campo = proximo_campo(&cursor);
            if (!converter_uint32(campo, &valor)) return false;
            solicitacao->valor = valor;
            return true;
        case ACAO_MESTRE_DEFINIR_INTERVALO_OTA:
        case ACAO_MESTRE_DEFINIR_INTERVALO_TELEMETRIA_OTA:
            if (!converter_uint32(proximo_campo(&cursor), &valor))
                return false;
            solicitacao->valor = valor;
            return true;
        case ACAO_MESTRE_DEFINIR_MARCHA:
        case ACAO_MESTRE_DEFINIR_VELOCIDADE_MAXIMA:
        case ACAO_MESTRE_DEFINIR_ACELERACAO_MAXIMA:
        case ACAO_MESTRE_DEFINIR_PARADA_REMOTA:
            if (!corpo[0]) return false;
            for (const char *p = corpo; *p; ++p) if (*p < '0' || *p > '9') return false;
            if (!converter_uint32(corpo, &valor)) return false;
            solicitacao->valor = valor;
            return true;
        case ACAO_MESTRE_DEFINIR_SIMULADOR:
            if (!converter_uint32(proximo_campo(&cursor), &valor) || valor > 1 ||
                cursor != NULL) return false;
            solicitacao->valor = valor;
            return true;
        case ACAO_MESTRE_PREPARAR_REDE_WIFI: {
            const char *ssid = proximo_campo(&cursor);
            const char *senha = proximo_campo(&cursor);
            if (ssid == NULL || senha == NULL || strlen(ssid) > 32 ||
                strlen(senha) > 63 ||
                strlen(ssid) + strlen(senha) + 2 >
                    sizeof(solicitacao->texto)) return false;
            strcpy(solicitacao->texto, ssid);
            strcpy(solicitacao->texto + strlen(ssid) + 1, senha);
            return true;
        }
        case ACAO_MESTRE_CONECTAR_REDE_WIFI_SALVA:
        case ACAO_MESTRE_EXCLUIR_REDE_WIFI_SALVA:
            if (!converter_uint32(proximo_campo(&cursor), &valor) ||
                valor < 1 || valor > 5) return false;
            solicitacao->alvo = (uint8_t)(valor - 1);
            return true;
        case ACAO_MESTRE_DEFINIR_WIFI_AUTOMATICO:
            if (!converter_uint32(proximo_campo(&cursor), &valor) ||
                valor > 1) return false;
            solicitacao->valor = valor;
            campo = proximo_campo(&cursor);
            if (campo == NULL || strlen(campo) >= sizeof(solicitacao->texto))
                return false;
            strcpy(solicitacao->texto, campo);
            return true;
        case ACAO_MESTRE_ALTERAR_REDE_WIFI_PRINCIPAL:
        case ACAO_MESTRE_RESTAURAR_REDE_WIFI_PRINCIPAL:
            campo = proximo_campo(&cursor);
            if (campo == NULL || strlen(campo) >= sizeof(solicitacao->texto))
                return false;
            strcpy(solicitacao->texto, campo);
            return true;
        case ACAO_MESTRE_DEFINIR_POLITICA_WIFI: {
            uint32_t valores[4] = {0};
            for (size_t indice = 0; indice < 4; ++indice) {
                if (!converter_uint32(proximo_campo(&cursor),
                                      &valores[indice])) return false;
            }
            solicitacao->valor = valores[0];
            solicitacao->valor_2 = valores[1];
            solicitacao->valor_3 = valores[2];
            solicitacao->valor_4 = valores[3];
            campo = proximo_campo(&cursor);
            if (campo == NULL || strlen(campo) >= sizeof(solicitacao->texto))
                return false;
            strcpy(solicitacao->texto, campo);
            return true;
        }
        default:
            return corpo[0] == '\0';
    }
}

static const char *descrever_resultado_comando(
    codigo_resposta_comando_t resultado)
{
    switch (resultado) {
        case RESPOSTA_COMANDO_OK: return "comando aceito";
        case RESPOSTA_COMANDO_NAO_SUPORTADO: return "comando não suportado";
        case RESPOSTA_COMANDO_CARGA_INVALIDA: return "dados do comando inválidos";
        case RESPOSTA_COMANDO_OCUPADO:
            return "operação indisponível no estado atual";
        case RESPOSTA_COMANDO_NEGADO: return "permissão negada";
        case RESPOSTA_COMANDO_ERRO_INTERNO: return "erro interno no mestre";
        case RESPOSTA_COMANDO_TIMEOUT: return "tempo de resposta esgotado";
    }
    return "resultado desconhecido";
}

static esp_err_t enviar_json_comando(httpd_req_t *requisicao,
                                     const char *estado_http,
                                     const char *json)
{
    if (estado_http != NULL) httpd_resp_set_status(requisicao, estado_http);
    httpd_resp_set_hdr(requisicao, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_type(requisicao, "application/json");
    return httpd_resp_send(requisicao, json, HTTPD_RESP_USE_STRLEN);
}

static void codificar_hexadecimal(char *destino, size_t capacidade,
                                  const uint8_t *dados, size_t tamanho)
{
    static const char DIGITOS[] = "0123456789ABCDEF";
    if (destino == NULL || capacidade == 0) return;
    size_t saida = 0;
    for (size_t indice = 0; indice < tamanho && saida + 2 < capacidade; ++indice) {
        destino[saida++] = DIGITOS[dados[indice] >> 4];
        destino[saida++] = DIGITOS[dados[indice] & 0x0Fu];
    }
    destino[saida] = '\0';
}

static esp_err_t processar_mestre_api(httpd_req_t *requisicao)
{
    char consulta_url[64] = {0};
    char consulta[32] = {0};
    if (httpd_req_get_url_query_str(requisicao, consulta_url,
                                    sizeof(consulta_url)) != ESP_OK ||
        httpd_query_key_value(consulta_url, "consulta", consulta,
                              sizeof(consulta)) != ESP_OK) {
        return enviar_json_comando(requisicao, "400 Bad Request",
                                   "{\"ok\":false,\"erro\":\"consulta ausente\"}");
    }
    const codigo_comando_t comando = localizar_consulta_mestre(consulta);
    if (comando == 0) {
        return enviar_json_comando(requisicao, "400 Bad Request",
                                   "{\"ok\":false,\"erro\":\"consulta desconhecida\"}");
    }
    const bool acao = comando_exige_confirmacao(comando);
    if ((acao && requisicao->method != HTTP_POST) ||
        (!acao && requisicao->method != HTTP_GET)) {
        return enviar_json_comando(requisicao, "405 Method Not Allowed",
            "{\"ok\":false,\"erro\":\"método HTTP não permitido\"}");
    }

    ESP_LOGI(TAG, "Painel solicitou '%s' ao mestre via %s", consulta,
             acao ? "POST" : "GET");

    const void *carga_solicitacao = NULL;
    uint16_t tamanho_carga_solicitacao = 0;
    solicitacao_acao_mestre_t solicitacao_acao = {0};
    solicitacao_rede_wifi_salva_t solicitacao_rede = {0};
    solicitacao_consulta_satelite_t solicitacao_satelite = {0};
    solicitacao_gestao_wifi_satelite_t solicitacao_wifi_satelite = {0};
    char corpo[256] = {0};
    TickType_t tempo_limite = pdMS_TO_TICKS(1500);
    if (comando == COMANDO_GERIR_WIFI_SATELITE) {
        if (receber_corpo_texto(requisicao, corpo, sizeof(corpo)) != ESP_OK)
            return enviar_json_comando(requisicao, "400 Bad Request",
                "{\"ok\":false,\"erro\":\"corpo inválido\"}");
        solicitacao_wifi_satelite.alvo = NO_COMANDO_VISITANTES;
        if (strcmp(consulta, "satelite_visitantes_limite") == 0) {
            uint32_t minutos = 0;
            if (!converter_uint32(corpo, &minutos) || minutos > 1440)
                return enviar_json_comando(requisicao, "400 Bad Request",
                    "{\"ok\":false,\"erro\":\"limite inválido\"}");
            solicitacao_wifi_satelite.acao = GESTAO_WIFI_SATELITE_LIMITE;
            solicitacao_wifi_satelite.limite_sessao_minutos = (uint16_t)minutos;
        } else {
            unsigned partes[6] = {0};
            int consumidos = 0;
            if (sscanf(corpo, "%2x:%2x:%2x:%2x:%2x:%2x%n",
                       &partes[0], &partes[1], &partes[2], &partes[3],
                       &partes[4], &partes[5], &consumidos) != 6 ||
                consumidos != (int)strlen(corpo))
                return enviar_json_comando(requisicao, "400 Bad Request",
                    "{\"ok\":false,\"erro\":\"MAC inválido\"}");
            solicitacao_wifi_satelite.acao = GESTAO_WIFI_SATELITE_DESCONECTAR;
            for (size_t i = 0; i < 6; ++i)
                solicitacao_wifi_satelite.mac[i] = (uint8_t)partes[i];
        }
        carga_solicitacao = &solicitacao_wifi_satelite;
        tamanho_carga_solicitacao = sizeof(solicitacao_wifi_satelite);
        tempo_limite = pdMS_TO_TICKS(3500);
    } else if (comando == COMANDO_EXECUTAR_ACAO_MESTRE) {
        if (receber_corpo_texto(requisicao, corpo, sizeof(corpo)) != ESP_OK ||
            !preparar_acao_mestre(consulta, corpo, &solicitacao_acao)) {
            return enviar_json_comando(
                requisicao, "400 Bad Request",
                "{\"ok\":false,\"erro\":\"dados da ação inválidos\"}");
        }
        if (solicitacao_acao.acao == ACAO_MESTRE_SINCRONIZAR_GOOGLE)
            solicitacao_acao.reservado =
                SOLICITACAO_ACAO_CONFIRMACAO_SINCRONIZACAO_DETALHADA;
        carga_solicitacao = &solicitacao_acao;
        tamanho_carga_solicitacao = sizeof(solicitacao_acao);
        if (solicitacao_acao.acao ==
                ACAO_MESTRE_CONECTAR_REDE_WIFI_PREPARADA ||
            solicitacao_acao.acao == ACAO_MESTRE_CONECTAR_REDE_WIFI_SALVA ||
            solicitacao_acao.acao ==
                ACAO_MESTRE_ALTERAR_REDE_WIFI_PRINCIPAL) {
            // A política do mestre permite sessões Wi-Fi de até 180 s.
            tempo_limite = pdMS_TO_TICKS(185000);
        }
    } else if (comando == COMANDO_OBTER_REDE_WIFI_SALVA) {
        char indice_texto[8] = {0};
        uint32_t indice = 0;
        if (httpd_query_key_value(consulta_url, "indice", indice_texto,
                                  sizeof(indice_texto)) != ESP_OK ||
            !converter_uint32(indice_texto, &indice) || indice < 1 ||
            indice > 5) {
            return enviar_json_comando(
                requisicao, "400 Bad Request",
                "{\"ok\":false,\"erro\":\"índice de rede inválido\"}");
        }
        solicitacao_rede.indice = (uint8_t)(indice - 1);
        carga_solicitacao = &solicitacao_rede;
        tamanho_carga_solicitacao = sizeof(solicitacao_rede);
    } else if (comando == COMANDO_CONSULTAR_SATELITE) {
        solicitacao_satelite.alvo = strstr(consulta, "visitantes") != NULL
                                         ? NO_COMANDO_VISITANTES
                                         : NO_COMANDO_EQUIPE;
        solicitacao_satelite.tipo = strstr(consulta, "versao") != NULL
                                        ? CONSULTA_SATELITE_VERSAO
                                        : strstr(consulta, "diagnostico") != NULL
                                        ? CONSULTA_SATELITE_DIAGNOSTICO
                                        : strstr(consulta, "clientes") != NULL
                                        ? CONSULTA_SATELITE_CLIENTES_WIFI
                                        : CONSULTA_SATELITE_PING;
        if (solicitacao_satelite.tipo == CONSULTA_SATELITE_CLIENTES_WIFI) {
            char inicio_texto[8] = {0};
            uint32_t inicio = 0;
            if (httpd_query_key_value(consulta_url, "inicio", inicio_texto,
                                      sizeof(inicio_texto)) != ESP_OK ||
                !converter_uint32(inicio_texto, &inicio) ||
                (inicio != 0 && inicio != 5))
                return enviar_json_comando(requisicao, "400 Bad Request",
                    "{\"ok\":false,\"erro\":\"página inválida\"}");
            solicitacao_satelite.reservado = (uint16_t)inicio;
        }
        carga_solicitacao = &solicitacao_satelite;
        tamanho_carga_solicitacao = sizeof(solicitacao_satelite);
        tempo_limite = pdMS_TO_TICKS(3500);
    }

    uint8_t resposta[COMANDO_TAMANHO_MAXIMO_CARGA] = {0};
    uint16_t tamanho_resposta = 0;
    codigo_resposta_comando_t resultado = RESPOSTA_COMANDO_TIMEOUT;
    informacoes_solicitacao_comando_t informacoes = {0};
    esp_err_t erro = servico_comandos_satelite_solicitar_detalhado(
        comando, carga_solicitacao, tamanho_carga_solicitacao, resposta,
        sizeof(resposta), &tamanho_resposta, &resultado, tempo_limite,
        &informacoes);
    if (erro == ESP_OK && resultado == RESPOSTA_COMANDO_CARGA_INVALIDA &&
        comando == COMANDO_EXECUTAR_ACAO_MESTRE &&
        solicitacao_acao.acao == ACAO_MESTRE_SINCRONIZAR_GOOGLE) {
        // O mestre antigo recusou a extensão antes de executar a ação.
        solicitacao_acao.reservado = 0;
        tamanho_resposta = 0;
        erro = servico_comandos_satelite_solicitar_detalhado(
            comando, &solicitacao_acao, sizeof(solicitacao_acao), resposta,
            sizeof(resposta), &tamanho_resposta, &resultado, tempo_limite,
            &informacoes);
    }
    if (erro != ESP_OK) {
        if (acao) {
            ESP_LOGW(TAG, "Mestre não confirmou a ação '%s': %s",
                     consulta, esp_err_to_name(erro));
        }
        char json[384];
        snprintf(json, sizeof(json),
                 "{\"ok\":false,\"consulta\":\"%s\",\"comando\":%u,"
                 "\"solicitacao\":%lu,\"tentativas\":%u,\"duracao_ms\":%lu,"
                 "\"erro\":\"%s\",\"codigo_esp\":%ld}",
                 consulta, (unsigned)comando,
                 (unsigned long)informacoes.identificador,
                 (unsigned)informacoes.tentativas,
                 (unsigned long)informacoes.duracao_ms,
                 esp_err_to_name(erro), (long)erro);
        return enviar_json_comando(
            requisicao, erro == ESP_ERR_TIMEOUT
                             ? "504 Gateway Timeout" : "502 Bad Gateway",
            json);
    }
    if (resultado != RESPOSTA_COMANDO_OK) {
        if (acao) {
            ESP_LOGW(TAG, "Mestre recusou a ação '%s' com resultado %u",
                     consulta, (unsigned)resultado);
        }
        char json[384];
        snprintf(json, sizeof(json),
                 "{\"ok\":false,\"consulta\":\"%s\",\"comando\":%u,"
                 "\"solicitacao\":%lu,\"resultado\":%u,\"tentativas\":%u,"
                 "\"duracao_ms\":%lu,\"erro\":\"%.63s\"}",
                 consulta, (unsigned)comando,
                 (unsigned long)informacoes.identificador, (unsigned)resultado,
                 (unsigned)informacoes.tentativas,
                 (unsigned long)informacoes.duracao_ms,
                 descrever_resultado_comando(resultado));
        return enviar_json_comando(requisicao, "409 Conflict", json);
    }

    char json[3072];
    int tamanho = -1;
    if (comando == COMANDO_PING && tamanho_resposta == sizeof(resposta_comando_ping_t)) {
        resposta_comando_ping_t dados;
        memcpy(&dados, resposta, sizeof(dados));
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"ping\",\"comando\":%u,\"solicitacao\":%lu,"
            "\"resultado\":0,\"tentativas\":%u,\"duracao_ms\":%lu,"
            "\"rtt_ms\":%lu,\"tempo_ativo_ms\":%lu}",
            (unsigned)comando, (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms,
            (unsigned long)informacoes.duracao_ms,
            (unsigned long)dados.tempo_ativo_ms);
    } else if (comando == COMANDO_OBTER_VERSAO &&
               tamanho_resposta == sizeof(resposta_comando_versao_t)) {
        resposta_comando_versao_t dados;
        char versao[65];
        memcpy(&dados, resposta, sizeof(dados));
        dados.versao[sizeof(dados.versao) - 1] = '\0';
        escapar_texto_json(versao, sizeof(versao), dados.versao);
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"versao\",\"comando\":%u,\"solicitacao\":%lu,"
            "\"resultado\":0,\"tentativas\":%u,\"duracao_ms\":%lu,"
            "\"firmware\":\"%s\",\"capacidades\":%lu,\"no\":%u}",
            (unsigned)comando, (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms, versao,
            (unsigned long)dados.capacidades, (unsigned)dados.no);
    } else if (comando == COMANDO_OBTER_SAUDE &&
               tamanho_resposta == sizeof(resposta_comando_saude_t)) {
        resposta_comando_saude_t dados;
        memcpy(&dados, resposta, sizeof(dados));
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"saude\",\"comando\":%u,\"solicitacao\":%lu,"
            "\"resultado\":0,\"tentativas\":%u,\"duracao_ms\":%lu,\"estado\":%u,"
            "\"causas_ativas\":%lu,\"tempo_ativo_ms\":%lu,\"heap_livre\":%lu,\"erros_historicos\":%lu}",
            (unsigned)comando, (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms, (unsigned)dados.estado,
            (unsigned long)dados.causas_ativas,
            (unsigned long)dados.tempo_ativo_ms, (unsigned long)dados.heap_livre,
            (unsigned long)dados.erros_historicos);
    } else if (comando == COMANDO_OBTER_DIAGNOSTICO_MESTRE &&
               tamanho_resposta == sizeof(resposta_diagnostico_mestre_t)) {
        resposta_diagnostico_mestre_t dados;
        char tarefa_cpu[31];
        char tarefa_pilha[31];
        memcpy(&dados, resposta, sizeof(dados));
        if (dados.versao_formato != COMANDO_FORMATO_DIAGNOSTICO_MESTRE) {
            return enviar_json_comando(
                requisicao, "502 Bad Gateway",
                "{\"ok\":false,\"erro\":\"formato de diagnóstico não suportado\"}");
        }
        dados.tarefa_cpu_pico[sizeof(dados.tarefa_cpu_pico) - 1] = '\0';
        dados.tarefa_menor_pilha[
            sizeof(dados.tarefa_menor_pilha) - 1] = '\0';
        escapar_texto_json(tarefa_cpu, sizeof(tarefa_cpu),
                           dados.tarefa_cpu_pico);
        escapar_texto_json(tarefa_pilha, sizeof(tarefa_pilha),
                           dados.tarefa_menor_pilha);
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"diagnostico_mestre\",\"comando\":%u,"
            "\"solicitacao\":%lu,\"resultado\":0,\"tentativas\":%u,\"duracao_ms\":%lu,"
            "\"formato\":%u,\"cpu_valida\":%s,\"cpu_total_decimos_percentual\":%u,"
            "\"cpu_nucleo_0_decimos_percentual\":%u,\"cpu_nucleo_1_decimos_percentual\":%u,"
            "\"cpu_tarefa_pico_decimos_percentual\":%u,\"cpu_tarefa_pico\":\"%s\","
            "\"temperatura_valida\":%s,\"temperatura_centesimos_c\":%d,"
            "\"temperatura_ultimo_erro\":%ld,\"tempo_ativo_ms\":%lu,"
            "\"heap_livre\":%lu,\"heap_minimo\":%lu,\"heap_interno_livre\":%lu,"
            "\"heap_interno_minimo\":%lu,\"heap_interno_maior_bloco\":%lu,"
            "\"heap_interno_menor_bloco\":%lu,\"psram_disponivel\":%s,"
            "\"psram_livre\":%lu,\"psram_minima\":%lu,\"psram_maior_bloco\":%lu,"
            "\"psram_menor_bloco\":%lu,\"pilha_valida\":%s,\"menor_pilha_livre_bytes\":%lu,"
            "\"tarefa_menor_pilha\":\"%s\",\"tarefas_ativas\":%u,\"causas_ativas\":%lu,"
            "\"motivo_ultimo_reset\":%lu,\"erros_sensores\":%lu,\"erros_i2c\":%lu,"
            "\"erros_uart\":%lu,\"descartes_uart\":%lu,\"falhas_ota\":%lu,"
            "\"falhas_supervisao\":%lu}",
            (unsigned)comando, (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms,
            (unsigned)dados.versao_formato,
            (dados.indicadores & DIAGNOSTICO_MESTRE_CPU_VALIDO) ? "true" : "false",
            (unsigned)dados.cpu_total_decimos_percentual,
            (unsigned)dados.cpu_nucleo_0_decimos_percentual,
            (unsigned)dados.cpu_nucleo_1_decimos_percentual,
            (unsigned)dados.cpu_tarefa_pico_decimos_percentual, tarefa_cpu,
            (dados.indicadores & DIAGNOSTICO_MESTRE_TEMPERATURA_VALIDA) ? "true" : "false",
            dados.temperatura_centesimos_c,
            (long)dados.temperatura_ultimo_erro,
            (unsigned long)dados.tempo_ativo_ms,
            (unsigned long)dados.heap_livre,
            (unsigned long)dados.menor_heap_livre,
            (unsigned long)dados.heap_interno_livre,
            (unsigned long)dados.menor_heap_interno_livre,
            (unsigned long)dados.maior_bloco_heap_interno,
            (unsigned long)dados.menor_maior_bloco_heap_interno,
            (dados.indicadores & DIAGNOSTICO_MESTRE_PSRAM_DISPONIVEL) ? "true" : "false",
            (unsigned long)dados.psram_livre,
            (unsigned long)dados.menor_psram_livre,
            (unsigned long)dados.maior_bloco_psram,
            (unsigned long)dados.menor_maior_bloco_psram,
            (dados.indicadores & DIAGNOSTICO_MESTRE_PILHA_VALIDA) ? "true" : "false",
            (unsigned long)dados.menor_pilha_livre_bytes, tarefa_pilha,
            (unsigned)dados.tarefas_ativas,
            (unsigned long)dados.causas_ativas,
            (unsigned long)dados.motivo_ultimo_reset,
            (unsigned long)dados.erros_sensores,
            (unsigned long)dados.erros_i2c,
            (unsigned long)dados.erros_uart,
            (unsigned long)dados.descartes_uart,
            (unsigned long)dados.falhas_ota,
            (unsigned long)dados.falhas_supervisao);
    } else if (comando == COMANDO_OBTER_TELEMETRIA &&
               tamanho_resposta == sizeof(resposta_comando_telemetria_t)) {
        resposta_comando_telemetria_t dados;
        memcpy(&dados, resposta, sizeof(dados));
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"telemetria\",\"comando\":%u,\"solicitacao\":%lu,"
            "\"resultado\":0,\"tentativas\":%u,\"duracao_ms\":%lu,\"sequencia\":%u,"
            "\"tempo_mestre_ms\":%lu,\"adc\":%u,\"velocidade_kmh\":%.2f,\"carga_percentual\":%.1f}",
            (unsigned)comando, (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms, (unsigned)dados.sequencia,
            (unsigned long)dados.tempo_mestre_ms,
            (unsigned)dados.tensao_adc_bruta,
            dados.velocidade_centesimos_kmh / 100.0,
            dados.carga_decimos_percentual / 10.0);
    } else if (comando == COMANDO_OBTER_ESTADO_UART &&
               tamanho_resposta == sizeof(resposta_comando_uart_t)) {
        resposta_comando_uart_t dados;
        memcpy(&dados, resposta, sizeof(dados));
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"uart\",\"comando\":%u,\"solicitacao\":%lu,"
            "\"resultado\":0,\"tentativas\":%u,\"duracao_ms\":%lu,\"envios\":%lu,"
            "\"recepcoes\":%lu,\"erros\":%lu,\"descartes\":%lu}",
            (unsigned)comando, (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms,
            (unsigned long)dados.envios, (unsigned long)dados.recepcoes,
            (unsigned long)dados.erros, (unsigned long)dados.descartes);
    } else if (comando == COMANDO_OBTER_ESTADO_OTA &&
               tamanho_resposta == sizeof(resposta_comando_ota_detalhada_t)) {
        resposta_comando_ota_detalhada_t dados;
        char mestre_atual[37], mestre_disponivel[37];
        char equipe_atual[37], equipe_disponivel[37];
        char visitantes_atual[37], visitantes_disponivel[37];
        memcpy(&dados, resposta, sizeof(dados));
        if (dados.versao_formato != COMANDO_FORMATO_OTA_DETALHADO) {
            ESP_LOGW(TAG, "Resposta OTA detalhada com formato desconhecido: %u",
                     (unsigned)dados.versao_formato);
            return enviar_json_comando(
                requisicao, "502 Bad Gateway",
                "{\"ok\":false,\"erro\":\"formato da resposta OTA não suportado\"}");
        }
        dados.versao_mestre_atual[sizeof(dados.versao_mestre_atual) - 1] = '\0';
        dados.versao_mestre_disponivel[sizeof(dados.versao_mestre_disponivel) - 1] = '\0';
        dados.versao_equipe_atual[sizeof(dados.versao_equipe_atual) - 1] = '\0';
        dados.versao_equipe_disponivel[sizeof(dados.versao_equipe_disponivel) - 1] = '\0';
        dados.versao_visitantes_atual[sizeof(dados.versao_visitantes_atual) - 1] = '\0';
        dados.versao_visitantes_disponivel[sizeof(dados.versao_visitantes_disponivel) - 1] = '\0';
        escapar_texto_json(mestre_atual, sizeof(mestre_atual), dados.versao_mestre_atual);
        escapar_texto_json(mestre_disponivel, sizeof(mestre_disponivel), dados.versao_mestre_disponivel);
        escapar_texto_json(equipe_atual, sizeof(equipe_atual), dados.versao_equipe_atual);
        escapar_texto_json(equipe_disponivel, sizeof(equipe_disponivel), dados.versao_equipe_disponivel);
        escapar_texto_json(visitantes_atual, sizeof(visitantes_atual), dados.versao_visitantes_atual);
        escapar_texto_json(visitantes_disponivel, sizeof(visitantes_disponivel), dados.versao_visitantes_disponivel);
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"ota\",\"comando\":%u,\"solicitacao\":%lu,"
            "\"resultado\":0,\"tentativas\":%u,\"duracao_ms\":%lu,\"formato\":%u,"
            "\"estado\":%u,\"alvo\":%u,\"falhas\":%lu,\"verificacoes\":%lu,"
            "\"mestre\":{\"estado\":%u,\"progresso_percentual\":%.1f,\"versao_atual\":\"%s\",\"versao_disponivel\":\"%s\"},"
            "\"equipe\":{\"estado\":%u,\"progresso_percentual\":%.1f,\"versao_atual\":\"%s\",\"versao_disponivel\":\"%s\"},"
            "\"visitantes\":{\"estado\":%u,\"progresso_percentual\":%.1f,\"versao_atual\":\"%s\",\"versao_disponivel\":\"%s\"}}",
            (unsigned)comando, (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms,
            (unsigned)dados.versao_formato, (unsigned)dados.estado,
            (unsigned)dados.alvo_ativo, (unsigned long)dados.falhas,
            (unsigned long)dados.verificacoes, (unsigned)dados.estado_mestre,
            dados.progresso_mestre_decimos / 10.0, mestre_atual, mestre_disponivel,
            (unsigned)dados.estado_equipe,
            dados.progresso_equipe_decimos / 10.0, equipe_atual, equipe_disponivel,
            (unsigned)dados.estado_visitantes,
            dados.progresso_visitantes_decimos / 10.0,
            visitantes_atual, visitantes_disponivel);
    } else if (comando == COMANDO_OBTER_ESTADO_OTA &&
               tamanho_resposta == sizeof(resposta_comando_ota_t)) {
        resposta_comando_ota_t dados;
        char atual[65], disponivel[65];
        memcpy(&dados, resposta, sizeof(dados));
        dados.versao_atual[sizeof(dados.versao_atual) - 1] = '\0';
        dados.versao_disponivel[sizeof(dados.versao_disponivel) - 1] = '\0';
        escapar_texto_json(atual, sizeof(atual), dados.versao_atual);
        escapar_texto_json(disponivel, sizeof(disponivel), dados.versao_disponivel);
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"ota\",\"comando\":%u,\"solicitacao\":%lu,"
            "\"resultado\":0,\"tentativas\":%u,\"duracao_ms\":%lu,\"formato\":1,"
            "\"estado\":%u,\"alvo\":%u,\"progresso_percentual\":%.1f,\"falhas\":%lu,"
            "\"versao_atual\":\"%s\",\"versao_disponivel\":\"%s\"}",
            (unsigned)comando, (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms,
            (unsigned)dados.estado, (unsigned)dados.alvo,
            dados.progresso_decimos / 10.0, (unsigned long)dados.falhas,
            atual, disponivel);
    } else if (comando == COMANDO_OBTER_ESTADO_WIFI &&
               tamanho_resposta == sizeof(resposta_comando_wifi_t)) {
        resposta_comando_wifi_t dados;
        char rede[67];
        memcpy(&dados, resposta, sizeof(dados));
        dados.rede[sizeof(dados.rede) - 1] = '\0';
        escapar_texto_json(rede, sizeof(rede), dados.rede);
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"wifi\",\"comando\":%u,\"solicitacao\":%lu,"
            "\"resultado\":0,\"tentativas\":%u,\"duracao_ms\":%lu,"
            "\"radio_ativo\":%s,\"conectado\":%s,\"rssi_dbm\":%d,\"canal\":%u,\"rede\":\"%s\"}",
            (unsigned)comando, (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms,
            dados.radio_ativo ? "true" : "false",
            dados.conectado ? "true" : "false", dados.rssi_dbm,
            (unsigned)dados.canal, rede);
    } else if (comando == COMANDO_OBTER_CONFIGURACAO_MESTRE &&
               tamanho_resposta == sizeof(resposta_configuracao_mestre_t)) {
        resposta_configuracao_mestre_t dados;
        memcpy(&dados, resposta, sizeof(dados));
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"configuracao\",\"comando\":%u,"
            "\"solicitacao\":%lu,\"resultado\":0,\"tentativas\":%u,"
            "\"duracao_ms\":%lu,\"relatorios_pausados\":%s,"
            "\"intervalos_relatorios_ms\":[%lu,%lu,%lu,%lu],"
            "\"intervalo_ota_minutos\":%lu,\"intervalo_telemetria_ota_ms\":%lu,"
            "\"cartao_disponivel\":%s,\"amostras_sd\":%lu,"
            "\"arquivos_pendentes\":%lu,\"arquivos_enviados\":%lu,"
            "\"quantidade_redes_wifi\":%u,\"wifi_automatico\":%s,"
            "\"tentativas_wifi\":%u,\"tempo_tentativa_wifi_ms\":%lu,"
            "\"intervalo_tentativas_wifi_ms\":%lu,\"tempo_limite_wifi_ms\":%lu}",
            (unsigned)comando, (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms,
            dados.relatorios_pausados ? "true" : "false",
            (unsigned long)dados.intervalos_relatorios_ms[0],
            (unsigned long)dados.intervalos_relatorios_ms[1],
            (unsigned long)dados.intervalos_relatorios_ms[2],
            (unsigned long)dados.intervalos_relatorios_ms[3],
            (unsigned long)dados.intervalo_ota_minutos,
            (unsigned long)dados.intervalo_telemetria_ota_ms,
            dados.cartao_disponivel ? "true" : "false",
            (unsigned long)dados.amostras_sd,
            (unsigned long)dados.arquivos_pendentes,
            (unsigned long)dados.arquivos_enviados,
            (unsigned)dados.quantidade_redes_wifi,
            dados.wifi_automatico ? "true" : "false",
            (unsigned)dados.tentativas_wifi,
            (unsigned long)dados.tempo_tentativa_wifi_ms,
            (unsigned long)dados.intervalo_tentativas_wifi_ms,
            (unsigned long)dados.tempo_limite_wifi_ms);
    } else if (comando == COMANDO_OBTER_CONTROLE_REMOTO &&
               tamanho_resposta == sizeof(estado_controle_remoto_t)) {
        estado_controle_remoto_t dados;
        memcpy(&dados, resposta, sizeof(dados));
        if (dados.formato != CONTROLE_REMOTO_FORMATO)
            return httpd_resp_send_err(requisicao, HTTPD_500_INTERNAL_SERVER_ERROR, "Formato de controle remoto incompatível");
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"formato\":%u,\"atuacao_habilitada\":%s,"
            "\"marcha_solicitada\":%u,\"parada_solicitada\":%s,"
            "\"velocidade_maxima_centesimos_kmh\":%lu,\"aceleracao_maxima_milesimos_ms2\":%lu,"
            "\"revisao\":%lu,\"alterado_em_ms\":%lu}",
            dados.formato, dados.atuacao_habilitada ? "true" : "false",
            dados.marcha_solicitada, dados.parada_solicitada ? "true" : "false",
            (unsigned long)dados.velocidade_maxima_centesimos_kmh,
            (unsigned long)dados.aceleracao_maxima_milesimos_ms2,
            (unsigned long)dados.revisao, (unsigned long)dados.alterado_em_ms);
    } else if (comando == COMANDO_OBTER_ESTADO_SIMULADOR &&
               tamanho_resposta == sizeof(resposta_simulador_mestre_t)) {
        resposta_simulador_mestre_t dados;
        memcpy(&dados, resposta, sizeof(dados));
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"simulador\",\"comando\":%u,"
            "\"solicitacao\":%lu,\"resultado\":0,\"tentativas\":%u,"
            "\"duracao_ms\":%lu,\"ativo\":%s,\"padrao_firmware\":%s,"
            "\"dinamica_percentual\":%u,\"amostras_geradas\":%lu}",
            (unsigned)comando, (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms,
            dados.ativo ? "true" : "false",
            dados.padrao_firmware ? "true" : "false",
            (unsigned)dados.dinamica_percentual,
            (unsigned long)dados.amostras_geradas);
    } else if (comando == COMANDO_OBTER_ESTADO_SINCRONIZACAO &&
               tamanho_resposta == sizeof(resposta_sincronizacao_mestre_t)) {
        resposta_sincronizacao_mestre_t dados;
        char arquivo[256];
        memcpy(&dados, resposta, sizeof(dados));
        if (dados.versao_formato != COMANDO_FORMATO_SINCRONIZACAO) {
            return enviar_json_comando(requisicao, "502 Bad Gateway",
                "{\"ok\":false,\"erro\":\"formato da sincronização não suportado\"}");
        }
        dados.arquivo_em_upload[sizeof(dados.arquivo_em_upload) - 1] = '\0';
        escapar_texto_json(arquivo, sizeof(arquivo), dados.arquivo_em_upload);
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"sincronizacao\",\"comando\":%u,"
            "\"solicitacao\":%lu,\"resultado\":0,\"tentativas\":%u,"
            "\"duracao_ms\":%lu,\"formato\":%u,\"iniciado\":%s,"
            "\"cartao_disponivel\":%s,\"em_andamento\":%s,"
            "\"aguardando_confirmacao\":%s,"
            "\"sincronizacoes_iniciadas\":%lu,\"sincronizacoes_finalizadas\":%lu,"
            "\"arquivos_pendentes\":%lu,\"arquivos_upload_total\":%lu,"
            "\"arquivos_upload_concluidos\":%lu,"
            "\"arquivo_atual_bytes_enviados\":%lu,"
            "\"arquivo_atual_bytes_total\":%lu,"
            "\"velocidade_upload_bytes_s\":%lu,"
            "\"tempo_restante_upload_s\":%lu,"
            "\"ultimo_resultado_sincronizacao\":%ld,"
            "\"ultimo_resultado_nome\":\"%s\","
            "\"arquivos_ultima_sincronizacao\":%lu,"
            "\"bytes_ultima_sincronizacao\":%lu,"
            "\"duracao_ultima_sincronizacao_ms\":%lu,"
            "\"fim_ultima_sincronizacao_ms\":%lu,"
            "\"arquivo_em_upload\":\"%s\"}",
            (unsigned)comando, (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms,
            (unsigned)dados.versao_formato,
            (dados.indicadores & SINCRONIZACAO_MESTRE_INICIADA) ? "true" : "false",
            (dados.indicadores & SINCRONIZACAO_MESTRE_CARTAO_DISPONIVEL) ? "true" : "false",
            (dados.indicadores & SINCRONIZACAO_MESTRE_EM_ANDAMENTO) ? "true" : "false",
            (dados.indicadores & SINCRONIZACAO_MESTRE_AGUARDANDO_CONFIRMACAO) ? "true" : "false",
            (unsigned long)dados.sincronizacoes_iniciadas,
            (unsigned long)dados.sincronizacoes_finalizadas,
            (unsigned long)dados.arquivos_pendentes,
            (unsigned long)dados.arquivos_upload_total,
            (unsigned long)dados.arquivos_upload_concluidos,
            (unsigned long)dados.arquivo_atual_bytes_enviados,
            (unsigned long)dados.arquivo_atual_bytes_total,
            (unsigned long)dados.velocidade_upload_bytes_s,
            (unsigned long)dados.tempo_restante_upload_s,
            (long)dados.ultimo_resultado_sincronizacao,
            esp_err_to_name((esp_err_t)dados.ultimo_resultado_sincronizacao),
            (unsigned long)dados.arquivos_ultima_sincronizacao,
            (unsigned long)dados.bytes_ultima_sincronizacao,
            (unsigned long)dados.duracao_ultima_sincronizacao_ms,
            (unsigned long)dados.fim_ultima_sincronizacao_ms, arquivo);
    } else if (comando == COMANDO_OBTER_REDE_WIFI_SALVA &&
               tamanho_resposta == sizeof(resposta_rede_wifi_salva_t)) {
        resposta_rede_wifi_salva_t dados;
        char ssid[67];
        memcpy(&dados, resposta, sizeof(dados));
        dados.ssid[sizeof(dados.ssid) - 1] = '\0';
        escapar_texto_json(ssid, sizeof(ssid), dados.ssid);
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"rede_wifi_salva\",\"comando\":%u,"
            "\"solicitacao\":%lu,\"resultado\":0,\"tentativas\":%u,"
            "\"duracao_ms\":%lu,\"indice\":%u,\"ssid\":\"%s\"}",
            (unsigned)comando, (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms,
            (unsigned)dados.indice + 1, ssid);
    } else if (comando == COMANDO_CONSULTAR_SATELITE &&
               tamanho_resposta == sizeof(resposta_consulta_diagnostico_satelite_t)) {
        resposta_consulta_diagnostico_satelite_t dados;
        memcpy(&dados, resposta, sizeof(dados));
        const resposta_diagnostico_satelite_t *d = &dados.diagnostico;
        if (dados.alvo != NO_COMANDO_VISITANTES ||
            dados.tipo != CONSULTA_SATELITE_DIAGNOSTICO ||
            d->versao_formato != COMANDO_FORMATO_DIAGNOSTICO_SATELITE)
            return enviar_json_comando(requisicao, "502 Bad Gateway",
                "{\"ok\":false,\"erro\":\"diagnóstico incompatível\"}");
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"%s\",\"rtt_ms\":%lu,"
            "\"cpu_valida\":%s,\"cpu_total_decimos_percentual\":%u,"
            "\"cpu_nucleo_0_decimos_percentual\":%u,\"cpu_nucleo_1_decimos_percentual\":%u,"
            "\"temperatura_valida\":%s,\"temperatura_centesimos_c\":%d,"
            "\"temperatura_ultimo_erro\":%ld,\"tempo_ativo_ms\":%lu,"
            "\"heap_interno_livre\":%lu,\"heap_interno_minimo\":%lu,"
            "\"psram_total\":%lu,\"psram_livre\":%lu,\"clientes_wifi\":%u,"
            "\"tarefas_ativas\":%u,\"tarefas_truncadas\":%s,\"menor_pilha_livre_bytes\":%lu,"
            "\"motivo_reinicio\":%u}", consulta, (unsigned long)dados.rtt_ms,
            (d->indicadores & 1U) ? "true" : "false",
            (unsigned)d->cpu_total_decimos_percentual,
            (unsigned)d->cpu_nucleo_0_decimos_percentual,
            (unsigned)d->cpu_nucleo_1_decimos_percentual,
            (d->indicadores & 2U) ? "true" : "false",
            (int)d->temperatura_centesimos_c, (long)d->temperatura_ultimo_erro,
            (unsigned long)d->tempo_ativo_ms,
            (unsigned long)d->heap_interno_livre,
            (unsigned long)d->heap_interno_minimo,
            (unsigned long)d->psram_total, (unsigned long)d->psram_livre,
            (unsigned)d->clientes_wifi, (unsigned)d->tarefas_ativas,
            (d->indicadores & 4U) ? "true" : "false",
            (unsigned long)d->menor_pilha_livre_bytes,
            (unsigned)d->motivo_reinicio);
    } else if (comando == COMANDO_CONSULTAR_SATELITE &&
               tamanho_resposta == sizeof(resposta_consulta_clientes_wifi_satelite_t)) {
        resposta_consulta_clientes_wifi_satelite_t dados;
        memcpy(&dados, resposta, sizeof(dados));
        const resposta_clientes_wifi_satelite_t *s = &dados.situacao;
        if (dados.alvo != NO_COMANDO_VISITANTES ||
            dados.tipo != CONSULTA_SATELITE_CLIENTES_WIFI ||
            s->quantidade_pagina > 5 || s->quantidade_total > 10 ||
            (s->inicio != 0 && s->inicio != 5))
            return enviar_json_comando(requisicao, "502 Bad Gateway",
                "{\"ok\":false,\"erro\":\"lista incompatível\"}");
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"%s\",\"rtt_ms\":%lu,"
            "\"limite_sessao_minutos\":%u,\"quantidade_total\":%u,"
            "\"inicio\":%u,\"clientes\":[", consulta,
            (unsigned long)dados.rtt_ms, (unsigned)s->limite_sessao_minutos,
            (unsigned)s->quantidade_total, (unsigned)s->inicio);
        for (uint8_t i = 0; i < s->quantidade_pagina && tamanho > 0 &&
                            tamanho < (int)sizeof(json); ++i) {
            const registro_cliente_wifi_satelite_t *c = &s->clientes[i];
            tamanho += snprintf(json + tamanho, sizeof(json) - (size_t)tamanho,
                "%s{\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
                "\"aid\":%u,\"conectado_ms\":%lu}", i ? "," : "",
                c->mac[0], c->mac[1], c->mac[2], c->mac[3], c->mac[4], c->mac[5],
                (unsigned)c->aid, (unsigned long)c->conectado_ms);
        }
        if (tamanho > 0 && tamanho < (int)sizeof(json))
            tamanho += snprintf(json + tamanho, sizeof(json) - (size_t)tamanho, "]}");
    } else if (comando == COMANDO_GERIR_WIFI_SATELITE &&
               tamanho_resposta == sizeof(resposta_gestao_wifi_satelite_t)) {
        resposta_gestao_wifi_satelite_t dados;
        memcpy(&dados, resposta, sizeof(dados));
        if (dados.alvo != NO_COMANDO_VISITANTES || !dados.aceito)
            return enviar_json_comando(requisicao, "502 Bad Gateway",
                "{\"ok\":false,\"erro\":\"ação não confirmada\"}");
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"%s\",\"aceito\":true,"
            "\"limite_sessao_minutos\":%u}", consulta,
            (unsigned)dados.limite_sessao_minutos);
    } else if (comando == COMANDO_CONSULTAR_SATELITE &&
               tamanho_resposta == sizeof(resposta_consulta_satelite_t)) {
        resposta_consulta_satelite_t dados;
        char versao[65];
        memcpy(&dados, resposta, sizeof(dados));
        dados.versao[sizeof(dados.versao) - 1] = '\0';
        escapar_texto_json(versao, sizeof(versao), dados.versao);
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"%s\",\"comando\":%u,"
            "\"solicitacao\":%lu,\"resultado\":0,\"tentativas\":%u,"
            "\"duracao_ms\":%lu,\"alvo\":%u,\"tipo\":%u,"
            "\"rtt_ms\":%lu,\"tempo_ativo_ms\":%lu,"
            "\"firmware\":\"%s\"}",
            consulta, (unsigned)comando,
            (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms, (unsigned)dados.alvo,
            (unsigned)dados.tipo, (unsigned long)dados.rtt_ms,
            (unsigned long)dados.tempo_ativo_ms, versao);
    } else if (acao && strcmp(consulta, "sincronizar_google") == 0 &&
               tamanho_resposta == sizeof(resposta_comando_acao_sincronizacao_t)) {
        resposta_comando_acao_sincronizacao_t dados;
        memcpy(&dados, resposta, sizeof(dados));
        if (!dados.confirmacao.aceito ||
            dados.confirmacao.comando != (uint16_t)comando ||
            dados.versao_formato != COMANDO_FORMATO_CONFIRMACAO_SINCRONIZACAO) {
            return enviar_json_comando(requisicao, "502 Bad Gateway",
                "{\"ok\":false,\"erro\":\"confirmação de sincronização inválida\"}");
        }
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"%s\",\"acao\":\"%s\",\"comando\":%u,"
            "\"solicitacao\":%lu,\"resultado\":0,\"tentativas\":%u,\"duracao_ms\":%lu,"
            "\"confirmada\":true,\"tempo_mestre_ms\":%lu,\"sincronizacao_id\":%lu,"
            "\"mensagem\":\"solicitação aceita pelo mestre\"}",
            consulta, consulta, (unsigned)comando,
            (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms,
            (unsigned long)dados.confirmacao.tempo_mestre_ms,
            (unsigned long)dados.sincronizacao_id);
    } else if (acao &&
               tamanho_resposta == sizeof(resposta_comando_acao_t)) {
        resposta_comando_acao_t dados;
        memcpy(&dados, resposta, sizeof(dados));
        if (!dados.aceito || dados.comando != (uint16_t)comando) {
            return enviar_json_comando(requisicao, "502 Bad Gateway",
                "{\"ok\":false,\"erro\":\"confirmação de ação inválida\"}");
        }
        ESP_LOGI(TAG, "Mestre confirmou a ação '%s'", consulta);
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"%s\",\"acao\":\"%s\",\"comando\":%u,"
            "\"solicitacao\":%lu,\"resultado\":0,\"tentativas\":%u,\"duracao_ms\":%lu,"
            "\"confirmada\":true,\"tempo_mestre_ms\":%lu,"
            "\"mensagem\":\"solicitação aceita pelo mestre\"}",
            consulta, consulta, (unsigned)comando,
            (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms,
            (unsigned long)dados.tempo_mestre_ms);
    } else if (acao && tamanho_resposta == 0) {
        // Compatibilidade durante a atualização gradual: a primeira versão
        // dos comandos confirmava a ação somente pelo código do cabeçalho.
        ESP_LOGW(TAG, "Mestre confirmou a ação '%s' no formato legado",
                 consulta);
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"%s\",\"acao\":\"%s\",\"comando\":%u,"
            "\"solicitacao\":%lu,\"resultado\":0,\"tentativas\":%u,\"duracao_ms\":%lu,"
            "\"confirmada\":true,\"formato\":\"legado\","
            "\"mensagem\":\"solicitação aceita pelo mestre\"}",
            consulta, consulta, (unsigned)comando,
            (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms);
    } else if (!acao) {
        char carga_hex[COMANDO_TAMANHO_MAXIMO_CARGA * 2 + 1];
        codificar_hexadecimal(carga_hex, sizeof(carga_hex), resposta,
                              tamanho_resposta);
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"%s\",\"comando\":%u,\"solicitacao\":%lu,"
            "\"resultado\":0,\"tentativas\":%u,\"duracao_ms\":%lu,"
            "\"formato\":\"binario\",\"tamanho_resposta\":%u,\"carga_hex\":\"%s\"}",
            consulta, (unsigned)comando,
            (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms,
            (unsigned)tamanho_resposta, carga_hex);
    }
    if (tamanho < 0 || tamanho >= (int)sizeof(json)) {
        return enviar_json_comando(requisicao, "502 Bad Gateway",
            "{\"ok\":false,\"erro\":\"resposta incompatível\"}");
    }
    return enviar_json_comando(requisicao, NULL, json);
}

static void tarefa_responder_mestre_api(void *argumento)
{
    httpd_req_t *requisicao = (httpd_req_t *)argumento;
    SemaphoreHandle_t vagas = requisicao->method == HTTP_POST
                                  ? vaga_api_mestre_acao
                                  : vagas_api_mestre_consultas;
    (void)processar_mestre_api(requisicao);
    httpd_req_async_handler_complete(requisicao);
    xSemaphoreGive(vagas);
    vTaskDelete(NULL);
}

static esp_err_t responder_mestre_api(httpd_req_t *requisicao)
{
    if (requisicao->method == HTTP_POST &&
        !autenticacao_comandos_autorizar(requisicao)) return ESP_OK;
    SemaphoreHandle_t vagas = requisicao->method == HTTP_POST
                                  ? vaga_api_mestre_acao
                                  : vagas_api_mestre_consultas;
    if (xSemaphoreTake(vagas, 0) != pdTRUE)
        return enviar_json_comando(requisicao, "503 Service Unavailable",
            "{\"ok\":false,\"erro\":\"mestre ocupado; tente novamente\"}");
    httpd_req_t *requisicao_assincrona = NULL;
    esp_err_t erro = httpd_req_async_handler_begin(
        requisicao, &requisicao_assincrona);
    if (erro != ESP_OK) {
        xSemaphoreGive(vagas);
        return erro;
    }
    if (xTaskCreate(tarefa_responder_mestre_api, "api_mestre", 8192,
                    requisicao_assincrona, 4, NULL) != pdPASS) {
        (void)enviar_json_comando(
            requisicao_assincrona, "503 Service Unavailable",
            "{\"ok\":false,\"erro\":\"sem recursos para processar comando\"}");
        httpd_req_async_handler_complete(requisicao_assincrona);
        xSemaphoreGive(vagas);
        return ESP_OK;
    }
    return ESP_OK;
}

static esp_err_t responder_telemetria(httpd_req_t *requisicao)
{
    dados_telemetria_t dados = { 0 };
    receptor_uart_obter_dados(&dados);
    uint32_t agora_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t idade_ms = dados.ultimo_recebimento_ms == 0 ? UINT32_MAX : agora_ms - dados.ultimo_recebimento_ms;
    char json[2048];
    int tamanho = snprintf(json, sizeof(json),
        "{\"valido\":%s,\"idade_ms\":%lu,\"sequencia\":%u,\"tempo_mestre_ms\":%lu,\"tensao_adc_bruta\":%u,"
        "\"aceleracao_x\":%d,\"aceleracao_y\":%d,\"aceleracao_z\":%d,\"giroscopio_x\":%d,\"giroscopio_y\":%d,\"giroscopio_z\":%d,"
        "\"aceleracao_x_milesimos_g\":%d,\"aceleracao_y_milesimos_g\":%d,\"aceleracao_z_milesimos_g\":%d,"
        "\"giroscopio_x_centesimos_graus_s\":%d,\"giroscopio_y_centesimos_graus_s\":%d,\"giroscopio_z_centesimos_graus_s\":%d,"
        "\"aceleracao_longitudinal_milesimos_g\":%d,\"temperatura_mpu_centesimos_c\":%d,\"estado_mpu\":%u,\"pacotes_validos\":%lu,\"pacotes_invalidos\":%lu,"
        "\"marcha\":%u,\"simulado\":%s,\"velocidade_centesimos_kmh\":%u,\"aceleracao_milesimos_ms2\":%d,"
        "\"acelerador_decimos_percentual\":%u,\"freio_decimos_percentual\":%u,\"tensao_pacote_mv\":%u,\"corrente_centesimos_a\":%d,\"carga_decimos_percentual\":%u,"
        "\"tensoes_celulas_mv\":[%u,%u,%u,%u],\"latitude_micrograus\":%ld,\"longitude_micrograus\":%ld,\"rumo_decimos_grau\":%u,"
        "\"distancias_cm\":[%u,%u,%u,%u,%u,%u,%u,%u],\"potencia_w\":%d,\"autonomia_decimos_km\":%u,\"percurso_metros\":%lu,\"odometro_metros\":%lu,"
        "\"falhas_crc\":%lu,\"falhas_cabecalho\":%lu,\"falhas_conteudo\":%lu,\"pacotes_perdidos\":%lu,\"pacotes_duplicados\":%lu,\"reinicios_mestre\":%lu,"
        "\"intervalo_medio_ms\":%lu,\"jitter_medio_ms\":%lu,\"latencia_relativa_ms\":%lu,\"taxa_pacotes_centesimos_hz\":%lu,\"taxa_perda_centesimos_percentual\":%lu}",
        dados.valido ? "true" : "false", (unsigned long)idade_ms,
        (unsigned)dados.sequencia,
        (unsigned long)dados.tempo_mestre_ms, (unsigned)dados.tensao_adc_bruta,
        dados.aceleracao_x,
        dados.aceleracao_y, dados.aceleracao_z, dados.giroscopio_x, dados.giroscopio_y,
        dados.giroscopio_z,
        dados.aceleracao_x_milesimos_g, dados.aceleracao_y_milesimos_g,
        dados.aceleracao_z_milesimos_g,
        dados.giroscopio_x_centesimos_graus_s,
        dados.giroscopio_y_centesimos_graus_s,
        dados.giroscopio_z_centesimos_graus_s,
        dados.aceleracao_longitudinal_milesimos_g,
        dados.temperatura_mpu_centesimos_c, (unsigned)dados.estado_mpu,
        (unsigned long)dados.pacotes_validos, (unsigned long)dados.pacotes_invalidos,
        (unsigned)dados.marcha, dados.simulado ? "true" : "false",
        (unsigned)dados.velocidade_centesimos_kmh, dados.aceleracao_milesimos_ms2,
        (unsigned)dados.acelerador_decimos_percentual, (unsigned)dados.freio_decimos_percentual,
        (unsigned)dados.tensao_pacote_mv, dados.corrente_centesimos_a,
        (unsigned)dados.carga_decimos_percentual,
        (unsigned)dados.tensoes_celulas_mv[0], (unsigned)dados.tensoes_celulas_mv[1],
        (unsigned)dados.tensoes_celulas_mv[2], (unsigned)dados.tensoes_celulas_mv[3],
        (long)dados.latitude_micrograus, (long)dados.longitude_micrograus,
        (unsigned)dados.rumo_decimos_grau,
        (unsigned)dados.distancias_cm[0], (unsigned)dados.distancias_cm[1],
        (unsigned)dados.distancias_cm[2], (unsigned)dados.distancias_cm[3],
        (unsigned)dados.distancias_cm[4], (unsigned)dados.distancias_cm[5],
        (unsigned)dados.distancias_cm[6], (unsigned)dados.distancias_cm[7],
        dados.potencia_w,
        (unsigned)dados.autonomia_decimos_km, (unsigned long)dados.percurso_metros,
        (unsigned long)dados.odometro_metros,
        (unsigned long)dados.falhas_crc, (unsigned long)dados.falhas_cabecalho,
        (unsigned long)dados.falhas_conteudo,
        (unsigned long)dados.pacotes_perdidos, (unsigned long)dados.pacotes_duplicados,
        (unsigned long)dados.reinicios_mestre,
        (unsigned long)dados.intervalo_medio_ms,
        (unsigned long)dados.jitter_medio_ms,
        (unsigned long)dados.latencia_relativa_ms,
        (unsigned long)dados.taxa_pacotes_centesimos_hz,
        (unsigned long)dados.taxa_perda_centesimos_percentual);
    if (tamanho < 0 || tamanho >= (int)sizeof(json)) {
        ESP_LOGE(TAG, "JSON de telemetria excedeu o buffer interno");
        return httpd_resp_send_err(requisicao, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "telemetria indisponivel");
    }
    httpd_resp_set_type(requisicao, "application/json");
    return httpd_resp_send(requisicao, json, tamanho);
}

static esp_err_t responder_diagnostico_pagina(httpd_req_t *requisicao)
{
    httpd_resp_set_hdr(requisicao, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_type(requisicao, "text/html; charset=utf-8");
    return httpd_resp_send(requisicao, PAGINA_WEB, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t responder_diagnostico_api(httpd_req_t *requisicao)
{
    dados_telemetria_t dados = {0};
    estatisticas_gerenciador_uart_t uart = {0};
    const estatisticas_servico_comandos_satelite_t comandos =
        servico_comandos_satelite_obter_estatisticas();
    const situacao_servico_ota_satelite_t ota =
        servico_ota_satelite_obter_situacao();
    const diagnostico_sistema_t sistema = diagnostico_sistema_obter();
    wifi_sta_list_t clientes = {0};
    uint8_t canal_wifi = 0;
    wifi_second_chan_t canal_secundario = WIFI_SECOND_CHAN_NONE;
    wifi_bandwidth_t largura_wifi = WIFI_BW_HT20;
    int8_t potencia_wifi = 0;
    receptor_uart_obter_dados(&dados);
    gerenciador_uart_obter_estatisticas(&uart);
    (void)esp_wifi_ap_get_sta_list(&clientes);
    (void)esp_wifi_get_channel(&canal_wifi, &canal_secundario);
    (void)esp_wifi_get_bandwidth(WIFI_IF_AP, &largura_wifi);
    (void)esp_wifi_get_max_tx_power(&potencia_wifi);
    const uint32_t agora_ms = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t idade_ms = dados.ultimo_recebimento_ms == 0
                                  ? UINT32_MAX : agora_ms - dados.ultimo_recebimento_ms;
    char versao_ota[sizeof(ota.versao_destino) * 2 + 1];
    escapar_texto_json(versao_ota, sizeof(versao_ota), ota.versao_destino);
    char tarefa_cpu[sizeof(sistema.cpu_tarefa_pico) * 2 + 1];
    char tarefa_pilha[sizeof(sistema.tarefa_menor_pilha) * 2 + 1];
    escapar_texto_json(tarefa_cpu, sizeof(tarefa_cpu),
                       sistema.cpu_tarefa_pico);
    escapar_texto_json(tarefa_pilha, sizeof(tarefa_pilha),
                       sistema.tarefa_menor_pilha);
    char json[4096];
    const int tamanho = snprintf(json, sizeof(json),
        "{\"valido\":%s,\"idade_ms\":%lu,\"versao_firmware\":\"%s\",\"versao_protocolo\":%u,"
        "\"compilacao_data\":\"%s\",\"compilacao_hora\":\"%s\",\"tempo_ativo_ms\":%lu,\"motivo_reinicio\":%u,"
        "\"heap_livre\":%lu,\"heap_minimo\":%lu,\"heap_maior_bloco\":%lu,\"clientes_wifi\":%u,"
        "\"heap_interno_total\":%lu,\"heap_interno_livre\":%lu,\"heap_interno_minimo\":%lu,\"heap_interno_maior_bloco\":%lu,"
        "\"psram_total\":%lu,\"psram_livre\":%lu,\"psram_minima\":%lu,\"psram_maior_bloco\":%lu,"
        "\"dma_livre\":%lu,\"dma_maior_bloco\":%lu,\"cpu_valida\":%s,\"cpu_total_decimos_percentual\":%u,"
        "\"cpu_nucleo_0_decimos_percentual\":%u,\"cpu_nucleo_1_decimos_percentual\":%u,\"cpu_tarefa_pico_decimos_percentual\":%u,"
        "\"cpu_tarefa_pico\":\"%s\",\"tarefas_ativas\":%lu,\"tarefas_truncadas\":%s,\"menor_pilha_livre_bytes\":%lu,\"tarefa_menor_pilha\":\"%s\","
        "\"temperatura_valida\":%s,\"temperatura_centesimos_c\":%ld,\"temperatura_ultimo_erro\":%ld,"
        "\"nome_local\":\"%s\",\"wifi_ssid\":\"%s\",\"wifi_ip\":\"%s\",\"wifi_largura_mhz\":%u,"
        "\"wifi_canal\":%u,\"wifi_potencia_quartos_dbm\":%d,"
        "\"sequencia\":%u,\"reinicios_mestre\":%lu,\"taxa_perda_centesimos_percentual\":%lu,"
        "\"taxa_pacotes_centesimos_hz\":%lu,\"intervalo_medio_ms\":%lu,\"jitter_medio_ms\":%lu,\"latencia_relativa_ms\":%lu,"
        "\"pacotes_validos\":%lu,\"pacotes_invalidos\":%lu,\"pacotes_perdidos\":%lu,\"pacotes_duplicados\":%lu,"
        "\"falhas_crc\":%lu,\"falhas_cabecalho\":%lu,\"falhas_conteudo\":%lu,"
        "\"uart_inicializada\":%s,\"uart_envios_ok\":%lu,\"uart_erros_envio\":%lu,\"uart_recepcoes_ok\":%lu,\"uart_erros_recepcao\":%lu,"
        "\"bytes_recebidos\":%llu,\"bytes_enviados\":%llu,\"uart_estouros_fifo\":%lu,\"uart_buffers_cheios\":%lu,"
        "\"uart_erros_quadro\":%lu,\"uart_erros_paridade\":%lu,\"uart_sinais_break\":%lu,\"uart_ultimo_erro_envio\":%ld,\"uart_ultimo_erro_recepcao\":%ld,"
        "\"comandos_solicitacoes_recebidas\":%lu,\"comandos_respostas_enviadas\":%lu,\"comandos_solicitacoes_enviadas\":%lu,"
        "\"comandos_ok\":%lu,\"comandos_repeticoes\":%lu,\"comandos_timeout\":%lu,\"comandos_invalidos\":%lu,\"comandos_ultimo_erro\":%ld,"
        "\"ota_iniciado\":%s,\"ota_recebendo\":%s,\"ota_sessao\":%lu,\"ota_tamanho_total\":%lu,\"ota_bytes_recebidos\":%lu,"
        "\"ota_quadros_recebidos\":%lu,\"ota_falhas\":%lu,\"ota_ultimo_erro\":%ld,\"ota_versao_destino\":\"%s\"}",
        dados.valido ? "true" : "false", (unsigned long)idade_ms,
        esp_app_get_description()->version,
        (unsigned)TELEMETRIA_VERSAO_PROTOCOLO,
        esp_app_get_description()->date, esp_app_get_description()->time,
        (unsigned long)agora_ms, (unsigned)esp_reset_reason(),
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)esp_get_minimum_free_heap_size(),
        (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        (unsigned)clientes.num,
        (unsigned long)sistema.heap_interno_total,
        (unsigned long)sistema.heap_interno_livre,
        (unsigned long)sistema.heap_interno_minimo,
        (unsigned long)sistema.heap_interno_maior_bloco,
        (unsigned long)sistema.psram_total,
        (unsigned long)sistema.psram_livre,
        (unsigned long)sistema.psram_minima,
        (unsigned long)sistema.psram_maior_bloco,
        (unsigned long)sistema.dma_livre,
        (unsigned long)sistema.dma_maior_bloco,
        sistema.cpu_valida ? "true" : "false",
        (unsigned)sistema.cpu_total_decimos_percentual,
        (unsigned)sistema.cpu_nucleo_0_decimos_percentual,
        (unsigned)sistema.cpu_nucleo_1_decimos_percentual,
        (unsigned)sistema.cpu_tarefa_pico_decimos_percentual,
        tarefa_cpu, (unsigned long)sistema.tarefas_ativas,
        sistema.tarefas_truncadas ? "true" : "false",
        (unsigned long)sistema.menor_pilha_livre_bytes, tarefa_pilha,
        sistema.temperatura_valida ? "true" : "false",
        (long)sistema.temperatura_centesimos_c,
        (long)sistema.temperatura_ultimo_erro,
        WIFI_IP_AP_TEXTO, WIFI_SSID, WIFI_IP_AP_TEXTO,
        largura_wifi == WIFI_BW_HT40 ? 40u : 20u, (unsigned)canal_wifi,
        (int)potencia_wifi, (unsigned)dados.sequencia,
        (unsigned long)dados.reinicios_mestre,
        (unsigned long)dados.taxa_perda_centesimos_percentual,
        (unsigned long)dados.taxa_pacotes_centesimos_hz,
        (unsigned long)dados.intervalo_medio_ms, (unsigned long)dados.jitter_medio_ms,
        (unsigned long)dados.latencia_relativa_ms,
        (unsigned long)dados.pacotes_validos, (unsigned long)dados.pacotes_invalidos,
        (unsigned long)dados.pacotes_perdidos, (unsigned long)dados.pacotes_duplicados,
        (unsigned long)dados.falhas_crc, (unsigned long)dados.falhas_cabecalho,
        (unsigned long)dados.falhas_conteudo,
        uart.inicializado ? "true" : "false",
        (unsigned long)uart.envios_ok, (unsigned long)uart.erros_envio,
        (unsigned long)uart.recepcoes_ok, (unsigned long)uart.erros_recepcao,
        (unsigned long long)uart.bytes_recebidos, (unsigned long long)uart.bytes_enviados,
        (unsigned long)uart.estouros_fifo, (unsigned long)uart.buffers_cheios,
        (unsigned long)uart.erros_quadro, (unsigned long)uart.erros_paridade,
        (unsigned long)uart.sinais_break, (long)uart.ultimo_erro_envio,
        (long)uart.ultimo_erro_recepcao,
        (unsigned long)comandos.solicitacoes_recebidas,
        (unsigned long)comandos.respostas_enviadas,
        (unsigned long)comandos.solicitacoes_enviadas,
        (unsigned long)comandos.respostas_recebidas,
        (unsigned long)comandos.repeticoes,
        (unsigned long)comandos.timeouts,
        (unsigned long)comandos.quadros_invalidos, (long)comandos.ultimo_erro,
        ota.iniciado ? "true" : "false", ota.recebendo ? "true" : "false",
        (unsigned long)ota.sessao, (unsigned long)ota.tamanho_total,
        (unsigned long)ota.bytes_recebidos,
        (unsigned long)ota.quadros_recebidos, (unsigned long)ota.falhas,
        (long)ota.ultimo_erro, versao_ota);
    if (tamanho < 0 || tamanho >= (int)sizeof(json)) {
        return httpd_resp_send_err(requisicao, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "diagnostico indisponivel");
    }
    httpd_resp_set_type(requisicao, "application/json");
    return httpd_resp_send(requisicao, json, tamanho);
}

static esp_err_t responder_redirecionamento_cativo(
    httpd_req_t *requisicao, httpd_err_code_t codigo)
{
    (void)codigo;
    static uint32_t redirecionamentos;
    if (++redirecionamentos == 1 || redirecionamentos % 20 == 0) {
        ESP_LOGI(TAG, "Portal HTTP: %lu redirecionamento(s); último caminho=%s",
                 (unsigned long)redirecionamentos, requisicao->uri);
    }
    httpd_resp_set_status(requisicao, "302 Found");
    httpd_resp_set_hdr(requisicao, "Location", URL_IP_LOCAL_EQUIPE);
    httpd_resp_set_hdr(requisicao, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_hdr(requisicao, "Connection", "close");
    return httpd_resp_send(requisicao, "Abrindo o painel local...",
                           HTTPD_RESP_USE_STRLEN);
}

esp_err_t servidor_web_iniciar(void)
{
    const esp_err_t erro_autenticacao = autenticacao_comandos_iniciar();
    if (erro_autenticacao != ESP_OK) {
        ESP_LOGE(TAG, "Autenticacao dos comandos indisponivel: %s",
                 esp_err_to_name(erro_autenticacao));
        return erro_autenticacao;
    }
    if (vagas_api_mestre_consultas == NULL)
        vagas_api_mestre_consultas = xSemaphoreCreateCounting(
            MAXIMO_CONSULTAS_MESTRE_SIMULTANEAS,
            MAXIMO_CONSULTAS_MESTRE_SIMULTANEAS);
    if (vaga_api_mestre_acao == NULL)
        vaga_api_mestre_acao = xSemaphoreCreateCounting(1, 1);
    if (vagas_api_mestre_consultas == NULL || vaga_api_mestre_acao == NULL)
        return ESP_ERR_NO_MEM;
    httpd_handle_t servidor = NULL;
    httpd_config_t configuracao = HTTPD_DEFAULT_CONFIG();
    configuracao.server_port = PORTA_HTTP;
    configuracao.max_uri_handlers = 12;
    // Reserva descritores para as demais tarefas de rede do ESP.
    configuracao.max_open_sockets = 10;
    configuracao.lru_purge_enable = true;
    configuracao.recv_wait_timeout = 10;
    configuracao.send_wait_timeout = 15;
    // Os manipuladores montam respostas de diagnóstico e OTA na pilha.
    configuracao.stack_size = 8192;
    esp_err_t erro = httpd_start(&servidor, &configuracao);
    if (erro != ESP_OK) {
        ESP_LOGE(TAG, "HTTP não iniciou na porta %u: %s",
                 (unsigned)PORTA_HTTP, esp_err_to_name(erro));
        return erro;
    }
    const httpd_uri_t rota_painel = { .uri = "/", .method = HTTP_GET, .handler = responder_painel };
    const httpd_uri_t rota_painel_direta = { .uri = "/painel", .method = HTTP_GET, .handler = responder_painel_equipe };
    const httpd_uri_t rota_api = { .uri = "/api/telemetria", .method = HTTP_GET, .handler = responder_telemetria };
    const httpd_uri_t rota_diagnostico = { .uri = "/diagnostico", .method = HTTP_GET, .handler = responder_diagnostico_pagina };
    const httpd_uri_t rota_api_diagnostico = { .uri = "/api/diagnostico", .method = HTTP_GET, .handler = responder_diagnostico_api };
    const httpd_uri_t rota_mestre = { .uri = "/mestre", .method = HTTP_GET, .handler = responder_mestre_pagina };
    const httpd_uri_t rota_api_mestre = { .uri = "/api/mestre", .method = HTTP_GET, .handler = responder_mestre_api };
    const httpd_uri_t rota_api_mestre_acao = { .uri = "/api/mestre", .method = HTTP_POST, .handler = responder_mestre_api };
    const httpd_uri_t rota_clientes_wifi = { .uri = "/api/wifi/clientes", .method = HTTP_GET, .handler = responder_clientes_wifi };
    const httpd_uri_t rota_limite_wifi = { .uri = "/api/wifi/limite", .method = HTTP_POST, .handler = responder_limite_wifi };
    const httpd_uri_t rota_desconexao_wifi = { .uri = "/api/wifi/desconectar", .method = HTTP_POST, .handler = responder_desconexao_wifi };
    const httpd_uri_t rota_acesso_comandos = { .uri = "/api/comandos/acesso", .method = HTTP_GET, .handler = responder_acesso_comandos };
    erro = httpd_register_err_handler(servidor, HTTPD_404_NOT_FOUND,
                                      responder_redirecionamento_cativo);
    if (erro == ESP_OK) erro = httpd_register_uri_handler(servidor, &rota_painel);
    if (erro == ESP_OK) erro = httpd_register_uri_handler(servidor, &rota_painel_direta);
    if (erro == ESP_OK) erro = httpd_register_uri_handler(servidor, &rota_api);
    if (erro == ESP_OK) erro = httpd_register_uri_handler(servidor, &rota_diagnostico);
    if (erro == ESP_OK) erro = httpd_register_uri_handler(servidor, &rota_api_diagnostico);
    if (erro == ESP_OK) erro = httpd_register_uri_handler(servidor, &rota_mestre);
    if (erro == ESP_OK) erro = httpd_register_uri_handler(servidor, &rota_api_mestre);
    if (erro == ESP_OK) erro = httpd_register_uri_handler(servidor, &rota_api_mestre_acao);
    if (erro == ESP_OK) erro = httpd_register_uri_handler(servidor, &rota_clientes_wifi);
    if (erro == ESP_OK) erro = httpd_register_uri_handler(servidor, &rota_limite_wifi);
    if (erro == ESP_OK) erro = httpd_register_uri_handler(servidor, &rota_desconexao_wifi);
    if (erro == ESP_OK) erro = httpd_register_uri_handler(servidor, &rota_acesso_comandos);
    if (erro != ESP_OK) {
        httpd_stop(servidor);
        ESP_LOGE(TAG, "Inicialização web incompleta; servidor encerrado: %s",
                 esp_err_to_name(erro));
        return erro;
    }
    ESP_LOGI(TAG, "Painéis HTTP disponíveis em %s, %s e %s",
             URL_IP_PORTAL_ENTRADA, URL_IP_LOCAL_EQUIPE, URL_IP_LOCAL_BASE "/diagnostico");
    return ESP_OK;
}
