#include "dispositivos/leitor_tensao.h"
#include "dispositivos/mpu6050.h"
#include "gerenciadores/gerenciador_i2c.h"
#include "gerenciadores/gerenciador_ota.h"
#include "gerenciadores/gerenciador_uart.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nucleo/configuracao_placa.h"
#include "servicos/servico_telemetria.h"
#include "servicos/servico_ota.h"

namespace {
constexpr char ETIQUETA[] = "mestre";
constexpr uint32_t TAMANHO_FLASH_ESPERADO = 16U * 1024U * 1024U;
constexpr size_t TAMANHO_PSRAM_ESPERADO = 8U * 1024U * 1024U;

struct DiagnosticoAplicacao {
    uint32_t erros_adc = 0;
    uint32_t erros_mpu6050 = 0;
    uint32_t erros_telemetria = 0;
    esp_err_t ultimo_erro_adc = ESP_OK;
    esp_err_t ultimo_erro_mpu6050 = ESP_OK;
    esp_err_t ultimo_erro_telemetria = ESP_OK;
};

DiagnosticoAplicacao diagnostico{};

esp_err_t verificar_hardware_n16r8() {
    uint32_t tamanho_flash = 0;
    const esp_err_t erro_flash = esp_flash_get_size(nullptr, &tamanho_flash);
    if (erro_flash != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Falha ao consultar a flash: %s", esp_err_to_name(erro_flash));
        return erro_flash;
    }

    const bool psram_iniciada = esp_psram_is_initialized();
    const size_t tamanho_psram = psram_iniciada ? esp_psram_get_size() : 0;
    ESP_LOGI(ETIQUETA, "Hardware detectado: flash=%lu MB | PSRAM=%lu MB (%s)",
             static_cast<unsigned long>(tamanho_flash / (1024U * 1024U)),
             static_cast<unsigned long>(tamanho_psram / (1024U * 1024U)),
             psram_iniciada ? "ativa" : "inativa");

    if (tamanho_flash != TAMANHO_FLASH_ESPERADO) {
        ESP_LOGE(ETIQUETA, "Flash inesperada: esperado=16 MB, detectado=%lu MB",
                 static_cast<unsigned long>(tamanho_flash / (1024U * 1024U)));
        return ESP_ERR_INVALID_SIZE;
    }
    if (!psram_iniciada || tamanho_psram != TAMANHO_PSRAM_ESPERADO) {
        ESP_LOGE(ETIQUETA, "PSRAM inesperada: esperado=8 MB Octal, detectado=%lu MB",
                 static_cast<unsigned long>(tamanho_psram / (1024U * 1024U)));
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

void registrar_estado_terminal(uint16_t tensao_bruta, const LeituraMpu6050& leitura_mpu) {
    static int64_t ultimo_relatorio_us = 0;
    const int64_t agora_us = esp_timer_get_time();
    if (agora_us - ultimo_relatorio_us < 1000000) return;
    ultimo_relatorio_us = agora_us;

    const EstatisticasTelemetria telemetria = servico_telemetria_obter_estatisticas();
    const EstatisticasUart uart_equipe =
        gerenciador_uart_obter_estatisticas(DestinoUart::Equipe);
    const EstatisticasUart uart_visitantes =
        gerenciador_uart_obter_estatisticas(DestinoUart::Visitantes);
    const EstatisticasI2c i2c = gerenciador_i2c_obter_estatisticas();
    const SituacaoOta ota = servico_ota_obter_situacao();
    ESP_LOGI(ETIQUETA,
             "TELEMETRIA seq=%u | ADC=%u (%.1f%%) | acc=[%d, %d, %d] giro=[%d, %d, %d]",
             telemetria.ultima_sequencia, tensao_bruta, (100.0f * tensao_bruta) / 4095.0f,
             leitura_mpu.aceleracao_x, leitura_mpu.aceleracao_y, leitura_mpu.aceleracao_z,
             leitura_mpu.giroscopio_x, leitura_mpu.giroscopio_y, leitura_mpu.giroscopio_z);
    ESP_LOGI(ETIQUETA, "UART equipe TX=%lu/%lluB erro=%lu RX=%lu/%lluB erro=%lu fisico=%lu",
             static_cast<unsigned long>(uart_equipe.envios_ok),
             static_cast<unsigned long long>(uart_equipe.bytes_enviados),
             static_cast<unsigned long>(uart_equipe.erros_envio),
             static_cast<unsigned long>(uart_equipe.recepcoes_ok),
             static_cast<unsigned long long>(uart_equipe.bytes_recebidos),
             static_cast<unsigned long>(uart_equipe.erros_recepcao),
             static_cast<unsigned long>(uart_equipe.erros_quadro + uart_equipe.erros_paridade));
    ESP_LOGI(ETIQUETA, "UART visitantes TX=%lu/%lluB erro=%lu RX=%lu/%lluB erro=%lu fisico=%lu",
             static_cast<unsigned long>(uart_visitantes.envios_ok),
             static_cast<unsigned long long>(uart_visitantes.bytes_enviados),
             static_cast<unsigned long>(uart_visitantes.erros_envio),
             static_cast<unsigned long>(uart_visitantes.recepcoes_ok),
             static_cast<unsigned long long>(uart_visitantes.bytes_recebidos),
             static_cast<unsigned long>(uart_visitantes.erros_recepcao),
             static_cast<unsigned long>(uart_visitantes.erros_quadro + uart_visitantes.erros_paridade));
    ESP_LOGI(ETIQUETA, "I2C operacoes=%lu erro=%lu ultimo=%s | aplicacao ADC=%lu MPU=%lu TX=%lu | checksum=0x%02X heap=%luB",
             static_cast<unsigned long>(i2c.operacoes_ok),
             static_cast<unsigned long>(i2c.erros), esp_err_to_name(i2c.ultimo_erro),
             static_cast<unsigned long>(diagnostico.erros_adc),
             static_cast<unsigned long>(diagnostico.erros_mpu6050),
             static_cast<unsigned long>(diagnostico.erros_telemetria),
             telemetria.ultimo_checksum,
             static_cast<unsigned long>(esp_get_free_heap_size()));
    ESP_LOGI(ETIQUETA, "OTA estado=%s atual=%s disponivel=%s falhas=%lu ultimo=%s",
             servico_ota_nome_estado(ota.estado),
             ota.versao_atual[0] == '\0' ? "-" : ota.versao_atual,
             ota.versao_disponivel[0] == '\0' ? "-" : ota.versao_disponivel,
             static_cast<unsigned long>(ota.falhas), esp_err_to_name(ota.ultimo_erro));
}

void tarefa_telemetria(void*) {
    uint16_t tensao_bruta = 0;
    LeituraMpu6050 leitura_mpu{};
    while (true) {
        esp_err_t erro_adc = leitor_tensao_ler(&tensao_bruta);
        esp_err_t erro_mpu = mpu6050_ler(&leitura_mpu);
        if (erro_adc != ESP_OK) {
            diagnostico.erros_adc++;
            diagnostico.ultimo_erro_adc = erro_adc;
        }
        if (erro_mpu != ESP_OK) {
            diagnostico.erros_mpu6050++;
            diagnostico.ultimo_erro_mpu6050 = erro_mpu;
        }
        if (erro_adc == ESP_OK && erro_mpu == ESP_OK) {
            esp_err_t erro_tx = servico_telemetria_enviar(tensao_bruta, leitura_mpu);
            if (erro_tx != ESP_OK) {
                diagnostico.erros_telemetria++;
                diagnostico.ultimo_erro_telemetria = erro_tx;
            }
        }
        registrar_estado_terminal(tensao_bruta, leitura_mpu);
        vTaskDelay(pdMS_TO_TICKS(configuracao::INTERVALO_TELEMETRIA_MS));
    }
}

esp_err_t inicializar_aplicacao() {
    esp_err_t erro = leitor_tensao_iniciar();
    if (erro != ESP_OK) return erro;
    erro = mpu6050_iniciar();
    if (erro != ESP_OK) return erro;
    return servico_telemetria_iniciar();
}
}  // namespace

extern "C" void app_main(void) {
    ESP_LOGI(ETIQUETA, "=== Mestre de telemetria UFSM-CS ===");
    ESP_LOGI(ETIQUETA, "Alvo: ESP32-S3-WROOM-1-N16R8");
    ESP_LOGI(ETIQUETA, "ADC GPIO %d | I2C SDA=%d SCL=%d | intervalo=%d ms",
             configuracao::PINO_LEITURA_TENSAO, configuracao::PINO_I2C_SDA,
             configuracao::PINO_I2C_SCL, configuracao::INTERVALO_TELEMETRIA_MS);
    esp_err_t erro = verificar_hardware_n16r8();
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Hardware incompativel com a configuracao N16R8: %s",
                 esp_err_to_name(erro));
        return;
    }

    erro = inicializar_aplicacao();
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Inicializacao interrompida: %s", esp_err_to_name(erro));
        gerenciador_ota_rejeitar_firmware_em_execucao();
        return;
    }

    if (xTaskCreate(tarefa_telemetria, "telemetria", 4096, nullptr, 5, nullptr) != pdPASS) {
        ESP_LOGE(ETIQUETA, "Nao foi possivel criar a tarefa de telemetria");
        gerenciador_ota_rejeitar_firmware_em_execucao();
        return;
    }

    erro = gerenciador_ota_confirmar_firmware_em_execucao();
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Nao foi possivel confirmar o firmware: %s", esp_err_to_name(erro));
        return;
    }

    erro = servico_ota_iniciar();
    if (erro != ESP_OK) {
        ESP_LOGW(ETIQUETA, "Telemetria ativa, mas OTA indisponivel: %s", esp_err_to_name(erro));
    }
}
