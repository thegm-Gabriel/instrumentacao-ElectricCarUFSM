// Publica a página de telemetria dos visitantes e seus dados em tempo real.
#include "servicos/servidor_web.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

#include "nucleo/configuracao.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "servicos/receptor_uart.h"
#include "servicos/painel_web.h"

static const char *TAG = "web_visitantes";

static esp_err_t responder_painel(httpd_req_t *requisicao)
{
    httpd_resp_set_type(requisicao, "text/html; charset=utf-8");
    return httpd_resp_send(requisicao, PAGINA_WEB_VISITANTES, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t responder_telemetria(httpd_req_t *requisicao)
{
    dados_telemetria_t dados = {0};
    receptor_uart_obter_dados(&dados);
    const uint32_t agora = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t idade = dados.ultimo_recebimento_ms == 0
                               ? UINT32_MAX
                               : agora - dados.ultimo_recebimento_ms;
    const bool aceleracao_disponivel = dados.valido &&
        (dados.simulado || (dados.estado_mpu & TELEMETRIA_MPU_ATIVO) != 0);
    char aceleracao[24] = "null";
    char tensao[24] = "null";
    char corrente[24] = "null";
    char carga[24] = "null";
    char potencia[24] = "null";
    char acelerador[8] = "null";
    char freio[8] = "null";
    char autonomia[8] = "null";
    if (aceleracao_disponivel) {
        snprintf(aceleracao, sizeof(aceleracao), "%d",
                 dados.aceleracao_milesimos_ms2);
    }
    // O modo real atual não adquire grandezas elétricas. Zero não é medição.
    if (dados.valido && dados.simulado) {
        // Os percentuais dos pedais também só são adquiridos pelo simulador atual.
        snprintf(acelerador, sizeof(acelerador), "%u", (unsigned)dados.acelerador_decimos_percentual);
        snprintf(freio, sizeof(freio), "%u", (unsigned)dados.freio_decimos_percentual);
        snprintf(tensao, sizeof(tensao), "%u", (unsigned)dados.tensao_pacote_mv);
        snprintf(corrente, sizeof(corrente), "%d", dados.corrente_centesimos_a);
        snprintf(carga, sizeof(carga), "%u", (unsigned)dados.carga_decimos_percentual);
        snprintf(potencia, sizeof(potencia), "%d", dados.potencia_w);
        snprintf(autonomia, sizeof(autonomia), "%u", (unsigned)dados.autonomia_decimos_km);
    }
    char json[768];
    const int tamanho = snprintf(
        json, sizeof(json),
        "{\"valido\":%s,\"idade_ms\":%lu,\"sequencia\":%u,"
        "\"simulado\":%s,\"velocidade_centesimos_kmh\":%u,"
        "\"acelerador_decimos_percentual\":%s,\"freio_decimos_percentual\":%s,"
        "\"aceleracao_disponivel\":%s,\"aceleracao_milesimos_ms2\":%s,"
        "\"tensao_pacote_mv\":%s,\"corrente_centesimos_a\":%s,"
        "\"carga_decimos_percentual\":%s,\"potencia_w\":%s,"
        "\"autonomia_decimos_km\":%s,\"percurso_metros\":%lu}",
        dados.valido ? "true" : "false", (unsigned long)idade,
        (unsigned)dados.sequencia, dados.simulado ? "true" : "false",
        (unsigned)dados.velocidade_centesimos_kmh,
        acelerador, freio,
        aceleracao_disponivel ? "true" : "false", aceleracao,
        tensao, corrente, carga, potencia,
        autonomia, (unsigned long)dados.percurso_metros);
    if (tamanho < 0 || tamanho >= (int)sizeof(json)) {
        return httpd_resp_send_err(requisicao,
                                   HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "telemetria indisponivel");
    }
    httpd_resp_set_type(requisicao, "application/json");
    httpd_resp_set_hdr(requisicao, "Cache-Control", "no-store");
    return httpd_resp_send(requisicao, json, tamanho);
}

static void tarefa_dns_cativo(void *argumento)
{
    const int soquete = (int)(intptr_t)argumento;
    while (true) {
        uint8_t pacote[512];
        struct sockaddr_in cliente;
        socklen_t tamanho_cliente = sizeof(cliente);
        int tamanho = recvfrom(soquete, pacote, sizeof(pacote), 0,
                               (struct sockaddr *)&cliente, &tamanho_cliente);
        if (tamanho < 17 || pacote[4] != 0 || pacote[5] != 1) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        int fim_nome = 12;
        bool nome_valido = false;
        while (fim_nome < tamanho) {
            const uint8_t comprimento = pacote[fim_nome++];
            if (comprimento == 0) {
                nome_valido = true;
                break;
            }
            if ((comprimento & 0xC0u) != 0 || comprimento > 63 ||
                fim_nome + comprimento > tamanho) break;
            fim_nome += comprimento;
        }
        if (!nome_valido || fim_nome + 4 > tamanho) continue;
        const bool ipv4 = pacote[fim_nome] == 0 && pacote[fim_nome + 1] == 1 &&
                          pacote[fim_nome + 2] == 0 && pacote[fim_nome + 3] == 1;
        const int fim_pergunta = fim_nome + 4;
        pacote[2] = (uint8_t)(0x84u | (pacote[2] & 0x01u));
        pacote[3] = 0;
        pacote[6] = 0;
        pacote[7] = ipv4 ? 1 : 0;
        pacote[8] = pacote[9] = pacote[10] = pacote[11] = 0;
        const uint8_t resposta[] = {
            0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 0, 0, 4, 192, 168, 4, 1,
        };
        if (ipv4 && fim_pergunta + (int)sizeof(resposta) <= (int)sizeof(pacote)) {
            memcpy(&pacote[fim_pergunta], resposta, sizeof(resposta));
            sendto(soquete, pacote, fim_pergunta + sizeof(resposta), 0,
                   (struct sockaddr *)&cliente, tamanho_cliente);
        } else if (!ipv4) {
            sendto(soquete, pacote, fim_pergunta, 0,
                   (struct sockaddr *)&cliente, tamanho_cliente);
        }
    }
}

static int criar_soquete_dns(void)
{
    int soquete = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (soquete < 0) {
        ESP_LOGW(TAG, "DNS local indisponível: errno=%d", errno);
        return -1;
    }
    struct sockaddr_in endereco = {
        .sin_family = AF_INET,
        .sin_port = htons(PORTA_DNS),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(soquete, (struct sockaddr *)&endereco, sizeof(endereco)) < 0) {
        ESP_LOGW(TAG, "Porta DNS indisponível: errno=%d", errno);
        close(soquete);
        return -1;
    }
    return soquete;
}

esp_err_t servidor_web_iniciar(void)
{
    httpd_handle_t servidor = NULL;
    httpd_config_t configuracao = HTTPD_DEFAULT_CONFIG();
    configuracao.server_port = PORTA_HTTP;
    configuracao.stack_size = 8192;
    esp_err_t erro = httpd_start(&servidor, &configuracao);
    if (erro != ESP_OK) {
        return erro;
    }
    const httpd_uri_t rota_painel = {
        .uri = "/", .method = HTTP_GET, .handler = responder_painel,
    };
    const httpd_uri_t rota_api = {
        .uri = "/api/telemetria",
        .method = HTTP_GET,
        .handler = responder_telemetria,
    };
    erro = httpd_register_uri_handler(servidor, &rota_painel);
    if (erro == ESP_OK) {
        erro = httpd_register_uri_handler(servidor, &rota_api);
    }
    if (erro != ESP_OK) {
        httpd_stop(servidor);
        ESP_LOGE(TAG, "Inicialização web incompleta; servidor encerrado: %s",
                 esp_err_to_name(erro));
        return erro;
    }
    const int soquete_dns = criar_soquete_dns();
    if (soquete_dns >= 0 &&
        xTaskCreate(tarefa_dns_cativo, "dns_visitantes", 4096,
                    (void *)(intptr_t)soquete_dns, 4, NULL) != pdPASS) {
        close(soquete_dns);
        ESP_LOGW(TAG, "Sem memória para DNS; acesso HTTP por IP mantido");
    }
    ESP_LOGI(TAG, "Painel de visitantes em http://%s", IP_PAINEL);
    return ESP_OK;
}
