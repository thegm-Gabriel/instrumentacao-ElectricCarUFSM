// Compõe os módulos do mestre e define sua política de inicialização.
#include "dispositivos/leitor_tensao.h"
#include "dispositivos/encoder_incremental.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nucleo/configuracao_placa.h"
#include "nvs_flash.h"
#include "servicos/servico_diagnostico.h"
#include "servicos/servico_enlace_satelites.h"
#include "servicos/servico_comandos.h"
#include "servicos/servico_aceleracao.h"
#include "servicos/servico_odometro.h"
#include "servicos/servico_ota.h"
#include "servicos/servico_painel_local.h"
#include "servicos/servico_registro_telemetria.h"
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

struct DisponibilidadeAquisicao {
    bool adc = false;
    bool mpu6050 = false;
    bool odometro = false;
    bool registro_sd = false;
};

DisponibilidadeAquisicao aquisicao{};

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
        // A NVS também contém odômetro e credenciais. Apagá-la automaticamente
        // para recuperar a inicialização destruiria dados persistentes sem aviso.
        ESP_LOGE(ETIQUETA,
                 "NVS requer manutenção (%s); armazenamento preservado",
                 esp_err_to_name(erro));
    }
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Não foi possível inicializar a NVS: %s",
                 esp_err_to_name(erro));
    }
    return erro;
}

void aplicar_leitura_mpu(DadosTelemetriaVeiculo& dados,
                         const EstadoServicoAceleracao& aceleracao) {
    const LeituraMpu6050& leitura = aceleracao.leitura;
    dados.aceleracao_x = leitura.aceleracao_x;
    dados.aceleracao_y = leitura.aceleracao_y;
    dados.aceleracao_z = leitura.aceleracao_z;
    dados.giroscopio_x = leitura.giroscopio_x;
    dados.giroscopio_y = leitura.giroscopio_y;
    dados.giroscopio_z = leitura.giroscopio_z;
    dados.aceleracao_x_g = leitura.aceleracao_x_g;
    dados.aceleracao_y_g = leitura.aceleracao_y_g;
    dados.aceleracao_z_g = leitura.aceleracao_z_g;
    dados.aceleracao_x_ms2 = leitura.aceleracao_x_ms2;
    dados.aceleracao_y_ms2 = leitura.aceleracao_y_ms2;
    dados.aceleracao_z_ms2 = leitura.aceleracao_z_ms2;
    dados.giroscopio_x_graus_s = leitura.giroscopio_x_graus_s;
    dados.giroscopio_y_graus_s = leitura.giroscopio_y_graus_s;
    dados.giroscopio_z_graus_s = leitura.giroscopio_z_graus_s;
    dados.temperatura_mpu_c = leitura.temperatura_c;
    dados.mpu_ativo = aceleracao.iniciado && aceleracao.ultimo_erro == ESP_OK;
    dados.mpu_calibrado = aceleracao.calibrado;
    dados.eixo_longitudinal_compativel =
        aceleracao.eixo_longitudinal_compativel;
    dados.eixo_longitudinal_mpu = configuracao::EIXO_LONGITUDINAL_MPU6050;
    dados.sentido_aceleracao_longitudinal =
        static_cast<int8_t>(aceleracao.sentido);
    dados.aceleracao_g = aceleracao.aceleracao_longitudinal_g;
    dados.aceleracao_ms2 = aceleracao.aceleracao_longitudinal_ms2;
}

void tarefa_telemetria(void*) {
    uint16_t tensao_bruta = 0;
    DadosTelemetriaVeiculo ultimos_dados{};
    int64_t ultimo_log_erro_envio_ms = -5000;
    int64_t ultimo_log_erro_odometro_ms = -5000;
    int64_t ultimo_log_erro_registro_ms = -5000;
    while (true) {
        servico_supervisao_alimentar(ModuloSupervisionado::TarefaTelemetria);
        if (configuracao::USAR_TELEMETRIA_SIMULADA) {
            const esp_err_t erro_simulador =
                servico_simulador_telemetria_gerar(&ultimos_dados);
            if (erro_simulador != ESP_OK) {
                ESP_LOGE(ETIQUETA, "Falha no simulador de telemetria: %s",
                         esp_err_to_name(erro_simulador));
            }
        } else {
            if (aquisicao.adc) {
                const esp_err_t erro_adc = leitor_tensao_ler(&tensao_bruta);
                (void)servico_diagnostico_registrar_resultado(
                    SensorDiagnosticado::Adc, erro_adc);
                if (erro_adc == ESP_OK) {
                    ultimos_dados.tensao_adc_bruta = tensao_bruta;
                }
            }
            if (aquisicao.mpu6050) {
                const EstadoServicoAceleracao estado_aceleracao =
                    servico_aceleracao_obter_estado();
                const esp_err_t erro_mpu = estado_aceleracao.ultimo_erro;
                (void)servico_diagnostico_registrar_resultado(
                    SensorDiagnosticado::Mpu6050, erro_mpu);
                ultimos_dados.mpu_ativo =
                    estado_aceleracao.iniciado && erro_mpu == ESP_OK;
                ultimos_dados.mpu_calibrado = estado_aceleracao.calibrado;
                ultimos_dados.eixo_longitudinal_compativel =
                    estado_aceleracao.eixo_longitudinal_compativel;
                ultimos_dados.eixo_longitudinal_mpu =
                    configuracao::EIXO_LONGITUDINAL_MPU6050;
                if (erro_mpu == ESP_OK && estado_aceleracao.leituras_ok > 0) {
                    aplicar_leitura_mpu(ultimos_dados, estado_aceleracao);
                } else {
                    ultimos_dados.sentido_aceleracao_longitudinal = 0;
                }
            }
            if (aquisicao.odometro) {
                const esp_err_t erro_odometro = servico_odometro_atualizar();
                const EstadoOdometro odometro = servico_odometro_obter_estado();
                ultimos_dados.velocidade_kmh = odometro.velocidade_kmh;
                ultimos_dados.percurso_km =
                    static_cast<float>(odometro.percurso_m / 1000.0);
                ultimos_dados.odometro_km =
                    static_cast<float>(odometro.odometro_m / 1000.0);
                if (erro_odometro != ESP_OK) {
                    const int64_t agora_ms = esp_timer_get_time() / 1000;
                    if (agora_ms - ultimo_log_erro_odometro_ms >= 5000) {
                        ESP_LOGW(ETIQUETA, "Falha ao atualizar odômetro: %s",
                                 esp_err_to_name(erro_odometro));
                        ultimo_log_erro_odometro_ms = agora_ms;
                    }
                }
            }
            ultimos_dados.simulado = false;
        }

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
        if (aquisicao.registro_sd) {
            const esp_err_t erro_registro =
                servico_registro_telemetria_registrar(ultimos_dados);
            if (erro_registro != ESP_OK && erro_registro != ESP_ERR_INVALID_STATE) {
                const int64_t agora_ms = esp_timer_get_time() / 1000;
                if (agora_ms - ultimo_log_erro_registro_ms >= 5000) {
                    ESP_LOGW(ETIQUETA, "Amostra histórica não enfileirada: %s",
                             esp_err_to_name(erro_registro));
                    ultimo_log_erro_registro_ms = agora_ms;
                }
            }
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
            const esp_err_t erro_associacao = servico_supervisao_associar_tarefa(
                ModuloSupervisionado::TarefaTelemetria,
                tarefa_telemetria_handle);
            if (erro_associacao != ESP_OK) {
                vTaskDelete(tarefa_telemetria_handle);
                tarefa_telemetria_handle = nullptr;
                return erro_associacao;
            }
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
    const esp_err_t erro_adc = leitor_tensao_iniciar();
    aquisicao.adc = erro_adc == ESP_OK;
    (void)servico_diagnostico_registrar_resultado(
        SensorDiagnosticado::Adc, erro_adc);
    if (erro_adc != ESP_OK) {
        ESP_LOGW(ETIQUETA, "ADC indisponível; telemetria seguirá degradada: %s",
                 esp_err_to_name(erro_adc));
    }

    ConfiguracaoServicoAceleracao configuracao_aceleracao{};
    configuracao_aceleracao.eixo_longitudinal =
        static_cast<EixoAceleracaoVeiculo>(
            configuracao::EIXO_LONGITUDINAL_MPU6050);
    configuracao_aceleracao.sinal_eixo_longitudinal =
        configuracao::SINAL_EIXO_LONGITUDINAL_MPU6050;
    configuracao_aceleracao.frequencia_amostragem_hz =
        configuracao::FREQUENCIA_AMOSTRAGEM_MPU6050_HZ;
    configuracao_aceleracao.frequencia_corte_filtro_hz =
        configuracao::FREQUENCIA_CORTE_ACELERACAO_HZ;
    configuracao_aceleracao.amostras_calibracao =
        configuracao::AMOSTRAS_CALIBRACAO_MPU6050;
    configuracao_aceleracao.limite_giroscopio_calibracao_graus_s =
        configuracao::LIMITE_GIRO_CALIBRACAO_MPU6050_GRAUS_S;
    configuracao_aceleracao.gravidade_minima_calibracao_g =
        configuracao::GRAVIDADE_MINIMA_CALIBRACAO_MPU6050_G;
    configuracao_aceleracao.gravidade_maxima_calibracao_g =
        configuracao::GRAVIDADE_MAXIMA_CALIBRACAO_MPU6050_G;
    configuracao_aceleracao.gravidade_maxima_eixo_longitudinal_g =
        configuracao::GRAVIDADE_MAXIMA_EIXO_LONGITUDINAL_G;
    configuracao_aceleracao.frequencia_correcao_gravidade_hz =
        configuracao::FREQUENCIA_CORRECAO_GRAVIDADE_HZ;
    configuracao_aceleracao.limite_aceleracao_correcao_gravidade_g =
        configuracao::LIMITE_ACELERACAO_CORRECAO_GRAVIDADE_G;
    configuracao_aceleracao.limiar_entrada_sentido_g =
        configuracao::LIMIAR_ENTRADA_SENTIDO_ACELERACAO_G;
    configuracao_aceleracao.limiar_saida_sentido_g =
        configuracao::LIMIAR_SAIDA_SENTIDO_ACELERACAO_G;
    configuracao_aceleracao.tempo_confirmacao_sentido_ms =
        configuracao::TEMPO_CONFIRMACAO_SENTIDO_ACELERACAO_MS;
    const esp_err_t erro_mpu =
        servico_aceleracao_iniciar(configuracao_aceleracao);
    aquisicao.mpu6050 = erro_mpu == ESP_OK;
    (void)servico_diagnostico_registrar_resultado(
        SensorDiagnosticado::Mpu6050, erro_mpu);
    if (erro_mpu != ESP_OK) {
        ESP_LOGW(ETIQUETA, "MPU6050 indisponível; telemetria seguirá degradada: %s",
                 esp_err_to_name(erro_mpu));
    }

    ConfiguracaoEncoderIncremental encoder_esquerdo{};
    encoder_esquerdo.pino_a = configuracao::PINO_ENCODER_ESQUERDO_A;
    encoder_esquerdo.pino_b = configuracao::PINO_ENCODER_ESQUERDO_B;
    encoder_esquerdo.pulsos_por_giro =
        configuracao::PULSOS_ENCODER_ESQUERDO_POR_GIRO;
    encoder_esquerdo.filtro_ruido_ns = configuracao::FILTRO_RUIDO_ENCODER_NS;
    const esp_err_t erro_encoder_esquerdo = encoder_incremental_iniciar(
        EncoderIncrementalId::RodaEsquerda, encoder_esquerdo);

    ConfiguracaoEncoderIncremental encoder_direito{};
    encoder_direito.pino_a = configuracao::PINO_ENCODER_DIREITO_A;
    encoder_direito.pino_b = configuracao::PINO_ENCODER_DIREITO_B;
    encoder_direito.pulsos_por_giro =
        configuracao::PULSOS_ENCODER_DIREITO_POR_GIRO;
    encoder_direito.filtro_ruido_ns = configuracao::FILTRO_RUIDO_ENCODER_NS;
    const esp_err_t erro_encoder_direito = encoder_incremental_iniciar(
        EncoderIncrementalId::RodaDireita, encoder_direito);
    const esp_err_t erro_encoder = erro_encoder_esquerdo != ESP_OK
                                       ? erro_encoder_esquerdo
                                       : erro_encoder_direito;
    (void)servico_diagnostico_registrar_resultado(
        SensorDiagnosticado::Encoder, erro_encoder);
    if (erro_encoder != ESP_OK) {
        ESP_LOGW(ETIQUETA,
                 "Odômetro indisponível; encoders incompletos (%s/%s)",
                 esp_err_to_name(erro_encoder_esquerdo),
                 esp_err_to_name(erro_encoder_direito));
        return ESP_OK;
    }

    ConfiguracaoOdometro configuracao_odometro{};
    configuracao_odometro.roda_esquerda.diametro_roda_m =
        configuracao::DIAMETRO_RODA_ESQUERDA_M;
    configuracao_odometro.roda_esquerda.giros_encoder_por_giro_roda =
        configuracao::GIROS_ENCODER_ESQUERDO_POR_GIRO_RODA;
    configuracao_odometro.roda_direita.diametro_roda_m =
        configuracao::DIAMETRO_RODA_DIREITA_M;
    configuracao_odometro.roda_direita.giros_encoder_por_giro_roda =
        configuracao::GIROS_ENCODER_DIREITO_POR_GIRO_RODA;
    configuracao_odometro.intervalo_persistencia_m =
        configuracao::INTERVALO_PERSISTENCIA_ODOMETRO_M;
    configuracao_odometro.velocidade_maxima_kmh =
        configuracao::VELOCIDADE_MAXIMA_ODOMETRO_KMH;
    configuracao_odometro.razao_minima_movimento_rodas =
        configuracao::RAZAO_MINIMA_MOVIMENTO_ENTRE_RODAS;
    configuracao_odometro.movimento_minimo_diagnostico_m =
        configuracao::MOVIMENTO_MINIMO_DIAGNOSTICO_RODAS_M;
    configuracao_odometro.amostras_divergentes_para_alerta =
        configuracao::AMOSTRAS_DIVERGENTES_PARA_ALERTA;
    const esp_err_t erro_odometro =
        servico_odometro_iniciar(configuracao_odometro);
    aquisicao.odometro = erro_odometro == ESP_OK;
    (void)servico_diagnostico_registrar_resultado(
        SensorDiagnosticado::Encoder, erro_odometro);
    if (erro_odometro != ESP_OK) {
        ESP_LOGW(ETIQUETA, "Odômetro indisponível; telemetria seguirá degradada: %s",
                 esp_err_to_name(erro_odometro));
    }
    return ESP_OK;
}

[[noreturn]] void rejeitar_firmware_e_encerrar(esp_err_t erro,
                                               const char* mensagem) {
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
    ESP_LOGW(ETIQUETA, "Reiniciando para impedir execução parcial do sistema");
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
    while (true) vTaskDelay(portMAX_DELAY);
}
}  // namespace

extern "C" void app_main(void) {
    ESP_LOGI(ETIQUETA, "=== Mestre de telemetria UFSM-CS ===");
    ESP_LOGI(ETIQUETA, "Alvo: ESP32-S3-WROOM-1-N16R8");
    ESP_LOGI(ETIQUETA, "ADC GPIO %d | I2C SDA=%d SCL=%d | intervalo=%d ms",
             configuracao::PINO_LEITURA_TENSAO, configuracao::PINO_I2C_SDA,
             configuracao::PINO_I2C_SCL, configuracao::INTERVALO_TELEMETRIA_MS);
    ESP_LOGI(ETIQUETA,
             "Encoders: esquerdo A/B=GPIO%d/%d, direito A/B=GPIO%d/%d",
             configuracao::PINO_ENCODER_ESQUERDO_A,
             configuracao::PINO_ENCODER_ESQUERDO_B,
             configuracao::PINO_ENCODER_DIREITO_A,
             configuracao::PINO_ENCODER_DIREITO_B);
    ESP_LOGW(ETIQUETA, "Fonte de telemetria: %s",
             configuracao::USAR_TELEMETRIA_SIMULADA ? "SIMULADOR" : "SENSORES REAIS");

    esp_err_t erro = verificar_hardware_n16r8();
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

    erro = servico_sinalizacao_iniciar();
    if (erro != ESP_OK) {
        ESP_LOGW(ETIQUETA, "Sistema seguirá sem LEDs e buzzer: %s",
                  esp_err_to_name(erro));
    }

    const esp_err_t erro_painel = servico_painel_local_iniciar();
    if (erro_painel != ESP_OK) {
        ESP_LOGW(ETIQUETA, "Sistema seguirá sem a tela local: %s",
                  esp_err_to_name(erro_painel));
        (void)servico_sinalizacao_notificar(
            EventoSinalizacao::AvisoTemporario);
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

    const esp_err_t erro_registro = servico_registro_telemetria_iniciar();
    aquisicao.registro_sd = erro_registro == ESP_OK;
    if (erro_registro != ESP_OK && erro_registro != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(ETIQUETA,
                 "Telemetria ao vivo ativa, mas histórico no SD indisponível: %s",
                 esp_err_to_name(erro_registro));
        servico_sinalizacao_notificar(EventoSinalizacao::AvisoTemporario);
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
    erro = servico_supervisao_associar_tarefa(
        ModuloSupervisionado::TarefaTelemetria, tarefa_telemetria_handle);
    if (erro != ESP_OK) {
        rejeitar_firmware_e_encerrar(
            erro, "Não foi possível associar a tarefa à supervisão");
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
        if (erro_wifi == ESP_OK) {
            const esp_err_t erro_encerramento = servico_wifi_encerrar_sessao();
            if (erro_encerramento != ESP_OK) {
                ESP_LOGW(ETIQUETA, "Falha ao encerrar sessão Wi-Fi: %s",
                         esp_err_to_name(erro_encerramento));
            }
        }
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

    // Confirma a imagem somente depois que o núcleo da aplicação e as
    // tentativas de inicialização dos serviços auxiliares foram concluídos.
    erro = servico_ota_confirmar_firmware_em_execucao();
    if (erro != ESP_OK) {
        rejeitar_firmware_e_encerrar(
            erro, "Não foi possível confirmar o firmware em execução");
        return;
    }
    servico_sinalizacao_notificar(EventoSinalizacao::SistemaPronto);
}
