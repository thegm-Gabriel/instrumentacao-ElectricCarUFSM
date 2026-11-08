#include "servicos/autenticacao_comandos.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

static const char *TAG = "auth_comandos";
static const char *ESPACO_NVS = "web_admin";
static const char *CHAVE_NVS = "token";
static char cabecalho_esperado[80];
static bool pronto;

static bool token_valido(const char *token)
{
    if (strlen(token) != 32) return false;
    for (size_t i = 0; i < 32; ++i) {
        const char c = token[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

static void codificar_basic(const char *token)
{
    static const char alfabeto[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char credencial[39];
    memcpy(credencial, "equipe:", 7);
    memcpy(credencial + 7, token, 32);
    const int tamanho = (int)sizeof(credencial);
    char *saida = cabecalho_esperado;
    memcpy(saida, "Basic ", 6);
    saida += 6;
    for (int i = 0; i < tamanho; i += 3) {
        const uint32_t a = (uint8_t)credencial[i];
        const uint32_t b = i + 1 < tamanho ? (uint8_t)credencial[i + 1] : 0;
        const uint32_t c = i + 2 < tamanho ? (uint8_t)credencial[i + 2] : 0;
        *saida++ = alfabeto[a >> 2];
        *saida++ = alfabeto[((a & 3u) << 4) | (b >> 4)];
        *saida++ = i + 1 < tamanho ? alfabeto[((b & 15u) << 2) | (c >> 6)] : '=';
        *saida++ = i + 2 < tamanho ? alfabeto[c & 63u] : '=';
    }
    *saida = '\0';
}

esp_err_t autenticacao_comandos_iniciar(void)
{
    if (pronto) return ESP_OK;
    nvs_handle_t nvs = 0;
    esp_err_t erro = nvs_open(ESPACO_NVS, NVS_READWRITE, &nvs);
    if (erro != ESP_OK) return erro;
    char token[33] = {0};
    size_t capacidade = sizeof(token);
    erro = nvs_get_str(nvs, CHAVE_NVS, token, &capacidade);
    if (erro == ESP_ERR_NVS_NOT_FOUND ||
        erro == ESP_ERR_NVS_INVALID_LENGTH ||
        (erro == ESP_OK && !token_valido(token))) {
        ESP_LOGW(TAG, "Chave administrativa ausente ou invalida; gerando nova chave");
        uint8_t aleatorio[16];
        esp_fill_random(aleatorio, sizeof(aleatorio));
        for (size_t i = 0; i < sizeof(aleatorio); ++i) {
            snprintf(token + i * 2, 3, "%02x", aleatorio[i]);
        }
        erro = nvs_set_str(nvs, CHAVE_NVS, token);
        if (erro == ESP_OK) erro = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (erro != ESP_OK) return erro;
    if (!token_valido(token)) return ESP_ERR_INVALID_RESPONSE;
    codificar_basic(token);
    pronto = true;
    ESP_LOGW(TAG, "Chave administrativa dos comandos (usuario equipe): %s", token);
    ESP_LOGW(TAG, "Guarde esta chave; ela nao e a senha Wi-Fi e aparece apenas no serial");
    return ESP_OK;
}

bool autenticacao_comandos_autorizar(httpd_req_t *requisicao)
{
    if (!pronto) {
        httpd_resp_set_status(requisicao, "503 Service Unavailable");
        httpd_resp_set_type(requisicao, "application/json");
        httpd_resp_send(requisicao,
            "{\"ok\":false,\"erro\":\"autenticacao administrativa indisponivel\"}",
            HTTPD_RESP_USE_STRLEN);
        return false;
    }
    char recebido[80] = {0};
    const bool presente = httpd_req_get_hdr_value_str(requisicao, "Authorization",
                                                      recebido, sizeof(recebido)) == ESP_OK;
    size_t diferenca = presente ? 0 : 1;
    for (size_t i = 0; i < sizeof(cabecalho_esperado); ++i) {
        diferenca |= (unsigned char)recebido[i] ^
                     (unsigned char)cabecalho_esperado[i];
    }
    if (diferenca == 0) return true;
    httpd_resp_set_status(requisicao, "401 Unauthorized");
    httpd_resp_set_hdr(requisicao, "Cache-Control", "no-store");
    httpd_resp_set_type(requisicao, "application/json");
    httpd_resp_send(requisicao,
        "{\"ok\":false,\"erro\":\"chave administrativa necessaria\"}",
        HTTPD_RESP_USE_STRLEN);
    return false;
}
