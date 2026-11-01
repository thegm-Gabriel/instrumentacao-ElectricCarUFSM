// Inicializa e supervisiona os serviços do satélite da equipe.
#include <stdbool.h>

#include "nucleo/configuracao.h"
#include "servicos/diagnostico_sistema.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "guarda_rollback_ota.h"
#include "servicos/receptor_uart.h"
#include "gerenciadores/gerenciador_wifi.h"
#include "servicos/servidor_web.h"
#include "servico_ota_satelite.h"

static const char *TAG = "main_equipe";

static void tratar_falha_de_inicializacao(const char *servico, esp_err_t erro)
{
    ESP_LOGE(TAG, "%s indisponível: %s", servico, esp_err_to_name(erro));
    if (guarda_ota_monitorando()) {
        ESP_LOGE(TAG, "Autoteste da nova imagem falhou; solicitando rollback");
        (void)guarda_ota_solicitar_rollback(TAG, "Serviço essencial indisponível");
    }
}

void app_main(void)
{
    guarda_ota_registrar_partida(TAG);
    ESP_LOGI(TAG, "Inicializando ESP da equipe; motivo do reset=%d",
             (int)esp_reset_reason());
    // Diagnóstico é complementar: temperatura indisponível não impede o painel.
    (void)diagnostico_sistema_iniciar();
    esp_err_t erro = gerenciador_wifi_iniciar();
    if (erro != ESP_OK) {
        tratar_falha_de_inicializacao("Wi-Fi", erro);
        return;
    }
    erro = servidor_web_iniciar();
    if (erro != ESP_OK) {
        tratar_falha_de_inicializacao("servidor web", erro);
        return;
    }
    ESP_LOGI(TAG, "Painel HTTP pronto em %s", URL_IP_PORTAL_ENTRADA);
    erro = receptor_uart_iniciar();
    if (erro != ESP_OK) {
        tratar_falha_de_inicializacao("UART", erro);
        return;
    }
    if (guarda_ota_monitorando()) {
        ESP_LOGI(TAG, "Observando firmware OTA antes da confirmação");
        vTaskDelay(pdMS_TO_TICKS(GUARDA_OTA_TEMPO_CONFIRMACAO_MS));
        erro = servico_ota_satelite_confirmar_firmware_em_execucao();
        if (erro != ESP_OK) {
            tratar_falha_de_inicializacao("confirmação OTA", erro);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(GUARDA_OTA_TEMPO_ESTABILIZACAO_MS -
                                GUARDA_OTA_TEMPO_CONFIRMACAO_MS));
        guarda_ota_encerrar_observacao(TAG);
    }
}
