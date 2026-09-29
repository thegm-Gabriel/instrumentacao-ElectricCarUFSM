#include "esp_log.h"
#include "receptor_uart.h"
#include "rede_wifi.h"
#include "servidor_web.h"
#include "servico_ota_satelite.h"

void app_main(void)
{
    ESP_ERROR_CHECK(rede_wifi_iniciar());
    ESP_ERROR_CHECK(receptor_uart_iniciar());
    ESP_ERROR_CHECK(servidor_web_iniciar());
    ESP_ERROR_CHECK(servico_ota_satelite_confirmar_firmware_em_execucao());
}
