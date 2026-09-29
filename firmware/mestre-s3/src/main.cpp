#include "dispositivos/leitor_tensao.h"
#include "dispositivos/mpu6050.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nucleo/configuracao_placa.h"
#include "nvs_flash.h"
#include "servicos/servico_diagnostico.h"
#include "servicos/servico_enlace_satelites.h"
#include "servicos/servico_comandos.h"
#include "servicos/servico_ota.h"
#include "servicos/servico_painel_local.h"
#include "servicos/servico_relatorios.h"
#include "servicos/servico_simulador_telemetria.h"
#include "servicos/servico_sinalizacao.h"
#include "servicos/servico_supervisao.h"
#include "servicos/servico_telemetria.h"
#include "servicos/servico_terminal.h"
#include "servicos/servico_wifi.h"

namespace {
constexpr char ETIQUETA[] = "mestre";
constexpr uint32_t MARGEM_SUPERVISAO_TELEMETRIA_MS = 5000;
TaskHandle_t tarefa_telemetria_handle = nullptr;

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

    if (tamanho_flash != configuracao::TAMANHO_FLASH_ESPERADO_BYTES) {
        ESP_LOGE(ETIQUETA, "Flash inesperada: esperado=16 MB, detectado=%lu MB",
                 static_cast<unsigned long>(tamanho_flash / (1024U * 1024U)));
        return ESP_ERR_INVALID_SIZE;
    }
    if (!psram_iniciada ||
        tamanho_psram != configuracao::TAMANHO_PSRAM_ESPERADO_BYTES) {
        ESP_LOGE(ETIQUETA, "PSRAM inesperada: esperado=8 MB Octal, detectado=%lu MB",
                 static_cast<unsigned long>(tamanho_psram / (1024U * 1024U)));
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t inicializar_armazenamento() {
    esp_err_t erro = nvs_flash_init();
    if (erro == ESP_ERR_NVS_NO_FREE_PAGES || erro == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(ETIQUETA, "NVS incompatível; reinicializando o armazenamento global");
        erro = nvs_flash_erase();
        if (erro == ESP_OK) erro = nvs_flash_init();
    }
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Não foi possível inicializar a NVS: %s",
                 esp_err_to_name(erro));
    }
    return erro;
}

DadosTelemetriaVeiculo converter_dados(uint16_t tensao_bruta,
                                       const LeituraMpu6050& leitura_mpu) {
    DadosTelemetriaVeiculo dados{};
    dados.tensao_adc_bruta = tensao_bruta;
    dados.aceleracao_x = leitura_mpu.aceleracao_x;
    dados.aceleracao_y = leitura_mpu.aceleracao_y;
    dados.aceleracao_z = leitura_mpu.aceleracao_z;
    dados.giroscopio_x = leitura_mpu.giroscopio_x;
    dados.giroscopio_y = leitura_mpu.giroscopio_y;
    dados.giroscopio_z = leitura_mpu.giroscopio_z;
    return dados;
}

void tarefa_telemetria(void*) {
    uint16_t tensao_bruta = 0;
    LeituraMpu6050 leitura_mpu{};
    DadosTelemetriaVeiculo ultimos_dados{};
    int64_t ultimo_log_erro_envio_ms = -5000;
    while (true) {
        servico_supervisao_alimentar(ModuloSupervisionado::TarefaTelemetria);
        esp_err_t erro_dados = ESP_OK;
        if (configuracao::USAR_TELEMETRIA_SIMULADA) {
            erro_dados = servico_simulador_telemetria_gerar(&ultimos_dados);
        } else {
            const esp_err_t erro_adc = leitor_tensao_ler(&tensao_bruta);
            const esp_err_t erro_mpu = mpu6050_ler(&leitura_mpu);
            servico_diagnostico_registrar_erro_adc(erro_adc);
            servico_diagnostico_registrar_erro_mpu6050(erro_mpu);
            erro_dados = erro_adc != ESP_OK ? erro_adc : erro_mpu;
            if (erro_dados == ESP_OK) {
                ultimos_dados = converter_dados(tensao_bruta, leitura_mpu);
            }
        }

        if (erro_dados == ESP_OK) {
            const esp_err_t erro_envio = servico_telemetria_enviar(ultimos_dados);
            if (erro_envio != ESP_OK) {
                const int64_t agora_ms = esp_timer_get_time() / 1000;
                if (agora_ms - ultimo_log_erro_envio_ms >= 5000) {
                    ESP_LOGW(ETIQUETA,
                             "Telemetria não enviada; o diagnóstico mantém a contagem do erro: %s",
                             esp_err_to_name(erro_envio));
                    ultimo_log_erro_envio_ms = agora_ms;
                }
            }
        } else {
            ESP_LOGE(ETIQUETA, "Falha ao obter dados de telemetria: %s",
                     esp_err_to_name(erro_dados));
        }
        servico_relatorios_atualizar_dados(ultimos_dados);
        ulTaskNotifyTake(
            pdTRUE,
            pdMS_TO_TICKS(servico_telemetria_obter_intervalo_atual()));
    }
}

esp_err_t criar_tarefa_telemetria() {
    if (tarefa_telemetria_handle != nullptr) return ESP_ERR_INVALID_STATE;
    if (xTaskCreate(tarefa_telemetria, "telemetria", 6144, nullptr, 5,
                    &tarefa_telemetria_handle) != pdPASS) {
        tarefa_telemetria_handle = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t recuperar_tarefa_telemetria() {
    if (tarefa_telemetria_handle == nullptr) {
        const esp_err_t erro = criar_tarefa_telemetria();
        if (erro == ESP_OK) {
            servico_supervisao_associar_tarefa(
                ModuloSupervisionado::TarefaTelemetria,
                tarefa_telemetria_handle);
        }
        return erro;
    }

    // A notificação acorda somente a espera controlada no fim do ciclo. Ela não
    // força a saída de mutexes ou drivers, evitando corromper seus estados.
    xTaskNotifyGive(tarefa_telemetria_handle);
    ESP_LOGW(ETIQUETA,
             "Tarefa de telemetria notificada para antecipar o próximo ciclo");
    return ESP_OK;
}

esp_err_t inicializar_aplicacao() {
    esp_err_t erro = servico_telemetria_iniciar();
    if (erro != ESP_OK) return erro;
    servico_comandos_definir_acao_resumo(servico_relatorios_solicitar_resumo);
    erro = servico_enlace_satelites_iniciar();
    if (erro != ESP_OK) return erro;
    if (configuracao::USAR_TELEMETRIA_SIMULADA) {
        return servico_simulador_telemetria_iniciar(
            configuracao::DINAMICA_SIMULACAO_PERCENTUAL);
    }
    erro = leitor_tensao_iniciar();
    if (erro != ESP_OK) return erro;
    return mpu6050_iniciar();
}

void rejeitar_firmware_e_encerrar(esp_err_t erro, const char* mensagem) {
    ESP_LOGE(ETIQUETA, "%s: %s", mensagem, esp_err_to_name(erro));
    const esp_err_t erro_sinalizacao =
        servico_sinalizacao_notificar(EventoSinalizacao::FalhaCritica);
    if (erro_sinalizacao != ESP_OK && erro_sinalizacao != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(ETIQUETA, "Falha ao sinalizar erro crítico: %s",
                 esp_err_to_name(erro_sinalizacao));
    }
    const esp_err_t erro_rejeicao = servico_ota_rejeitar_firmware_em_execucao();
    if (erro_rejeicao != ESP_OK && erro_rejeicao != ESP_ERR_NOT_FOUND) {
        ESP_LOGE(ETIQUETA, "Falha ao rejeitar firmware pendente: %s",
                 esp_err_to_name(erro_rejeicao));
    }
}
}  // namespace

extern "C" void app_main(void) {
    ESP_LOGI(ETIQUETA, "=== Mestre de telemetria UFSM-CS ===");
    ESP_LOGI(ETIQUETA, "Alvo: ESP32-S3-WROOM-1-N16R8");
    ESP_LOGI(ETIQUETA, "ADC GPIO %d | I2C SDA=%d SCL=%d | intervalo=%d ms",
             configuracao::PINO_LEITURA_TENSAO, configuracao::PINO_I2C_SDA,
             configuracao::PINO_I2C_SCL, configuracao::INTERVALO_TELEMETRIA_MS);
    ESP_LOGW(ETIQUETA, "Fonte de telemetria: %s",
             configuracao::USAR_TELEMETRIA_SIMULADA ? "SIMULADOR" : "SENSORES REAIS");

    esp_err_t erro = servico_sinalizacao_iniciar();
    if (erro != ESP_OK) {
        ESP_LOGW(ETIQUETA, "Sistema seguirá sem LEDs e buzzer: %s",
                 esp_err_to_name(erro));
    }

    const esp_err_t erro_painel = servico_painel_local_iniciar();
    if (erro_painel != ESP_OK) {
        ESP_LOGW(ETIQUETA, "Sistema seguirá sem a tela local: %s",
                 esp_err_to_name(erro_painel));
        servico_sinalizacao_notificar(EventoSinalizacao::AvisoTemporario);
    }

    erro = verificar_hardware_n16r8();
    if (erro != ESP_OK) {
        rejeitar_firmware_e_encerrar(
            erro, "Hardware incompatível com a configuração N16R8");
        return;
    }

    erro = inicializar_armazenamento();
    if (erro != ESP_OK) {
        rejeitar_firmware_e_encerrar(erro, "Inicialização do armazenamento interrompida");
        return;
    }

    erro = servico_supervisao_iniciar();
    if (erro != ESP_OK) {
        rejeitar_firmware_e_encerrar(erro, "Inicialização da supervisão interrompida");
        return;
    }

    erro = inicializar_aplicacao();
    if (erro != ESP_OK) {
        rejeitar_firmware_e_encerrar(erro, "Inicialização da aplicação interrompida");
        return;
    }

    erro = criar_tarefa_telemetria();
    if (erro != ESP_OK) {
        rejeitar_firmware_e_encerrar(erro,
                                     "Não foi possível criar a tarefa de telemetria");
        return;
    }
    erro = servico_supervisao_registrar(
        ModuloSupervisionado::TarefaTelemetria, "tarefa de telemetria",
        configuracao::INTERVALO_TELEMETRIA_OTA_MAXIMO_MS +
            MARGEM_SUPERVISAO_TELEMETRIA_MS,
        recuperar_tarefa_telemetria);
    if (erro != ESP_OK) {
        rejeitar_firmware_e_encerrar(erro, "Não foi possível supervisionar a telemetria");
        return;
    }
    servico_supervisao_associar_tarefa(
        ModuloSupervisionado::TarefaTelemetria, tarefa_telemetria_handle);

    erro = servico_ota_confirmar_firmware_em_execucao();
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Não foi possível confirmar o firmware: %s",
                 esp_err_to_name(erro));
        servico_sinalizacao_notificar(EventoSinalizacao::FalhaCritica);
        return;
    }

    const esp_err_t erro_wifi = servico_wifi_iniciar();
    if (erro_wifi != ESP_OK) {
        ESP_LOGW(ETIQUETA, "Telemetria ativa, mas Wi-Fi indisponível: %s",
                 esp_err_to_name(erro_wifi));
        servico_sinalizacao_notificar(EventoSinalizacao::AvisoTemporario);
    }

    erro = servico_ota_iniciar();
    if (erro != ESP_OK) {
        ESP_LOGW(ETIQUETA, "Telemetria ativa, mas OTA indisponível: %s",
                 esp_err_to_name(erro));
        servico_sinalizacao_notificar(EventoSinalizacao::AvisoTemporario);
        if (erro_wifi == ESP_OK) servico_wifi_encerrar_sessao();
    }

    erro = servico_terminal_iniciar();
    if (erro != ESP_OK) {
        ESP_LOGW(ETIQUETA, "Sistema ativo, mas menu serial indisponível: %s",
                 esp_err_to_name(erro));
        servico_sinalizacao_notificar(EventoSinalizacao::AvisoTemporario);
    }

    erro = servico_relatorios_iniciar();
    if (erro != ESP_OK) {
        ESP_LOGW(ETIQUETA,
                 "Sistema ativo, mas relatórios periódicos indisponíveis: %s",
                 esp_err_to_name(erro));
        servico_sinalizacao_notificar(EventoSinalizacao::AvisoTemporario);
    }

    erro = servico_diagnostico_iniciar();
    if (erro != ESP_OK) {
        ESP_LOGW(ETIQUETA, "Sistema ativo, mas diagnóstico periódico indisponível: %s",
                 esp_err_to_name(erro));
        servico_sinalizacao_notificar(EventoSinalizacao::AvisoTemporario);
    }
    servico_sinalizacao_notificar(EventoSinalizacao::SistemaPronto);
}
