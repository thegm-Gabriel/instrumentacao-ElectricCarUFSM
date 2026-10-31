// Inicializa e supervisiona os serviços do satélite da equipe.
#include <stdbool.h>

#include "nucleo/configuracao.h"
#include "servicos/diagnostico_sistema.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "servicos/receptor_uart.h"
#include "gerenciadores/gerenciador_wifi.h"
#include "servicos/servidor_web.h"
#include "servico_ota_satelite.h"

static const char *TAG = "main_equipe";

static bool firmware_pendente_de_verificacao(void)
{
    const esp_partition_t *particao = esp_ota_get_running_partition();
    esp_ota_img_states_t estado = ESP_OTA_IMG_UNDEFINED;
    return particao != NULL &&
           esp_ota_get_state_partition(particao, &estado) == ESP_OK &&
           estado == ESP_OTA_IMG_PENDING_VERIFY;
}

static void tratar_falha_de_inicializacao(const char *servico, esp_err_t erro)
{
    ESP_LOGE(TAG, "%s indisponível: %s", servico, esp_err_to_name(erro));
    if (firmware_pendente_de_verificacao()) {
        ESP_LOGE(TAG, "Autoteste da nova imagem falhou; solicitando rollback");
        const esp_err_t erro_rollback =
            esp_ota_mark_app_invalid_rollback_and_reboot();
        ESP_LOGE(TAG, "Não foi possível executar rollback: %s",
                 esp_err_to_name(erro_rollback));
    }
}

void app_main(void)
{
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
    erro = servico_ota_satelite_confirmar_firmware_em_execucao();
    if (erro != ESP_OK) tratar_falha_de_inicializacao("confirmação OTA", erro);
}
