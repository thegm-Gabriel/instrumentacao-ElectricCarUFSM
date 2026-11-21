#include "servicos/veiculo/aquisicao.h"

#include "dispositivos/entradas/encoder.h"
#include "dispositivos/entradas/ina228.h"
#include "dispositivos/entradas/adc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nucleo/configuracao/placa.h"
#include "nucleo/configuracao/tarefas.h"
#include "nucleo/configuracao/comunicacao.h"
#include "nucleo/configuracao/diagnostico.h"
#include "nucleo/configuracao/veiculo.h"
#include "servicos/veiculo/aceleracao.h"
#include "servicos/veiculo/odometro.h"
#include "servicos/veiculo/simulador.h"
#include "servicos/comunicacao/telemetria_uart.h"
#include "servicos/manutencao/supervisao.h"

namespace {
constexpr char ETIQUETA[] = "mestre";
TaskHandle_t tarefa_telemetria_handle = nullptr;

struct DisponibilidadeAquisicao {
    bool adc = false;
    bool ina228 = false;
    bool mpu6050 = false;
    bool odometro = false;
};

DisponibilidadeAquisicao aquisicao{};

constexpr uint32_t bit_sensor(SensorControlado sensor) {
    return 1u << static_cast<uint8_t>(sensor);
}

constexpr uint32_t TODOS = bit_sensor(SensorControlado::Quantidade) - 1u;
uint32_t sensores_ativos = TODOS;
portMUX_TYPE trava_sensores = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE trava_amostra = portMUX_INITIALIZER_UNLOCKED;
DadosTelemetriaVeiculo ultima_amostra{};
bool amostra_disponivel = false;
RegistradorResultadoSensor registrar_resultado_sensor = nullptr;
ConsumidorAmostraLocal consumir_amostra_local = nullptr;

uint32_t obter_mascara_sensores() {
    portENTER_CRITICAL(&trava_sensores);
    const uint32_t copia = sensores_ativos;
    portEXIT_CRITICAL(&trava_sensores);
    return copia;
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

void limpar_leitura_mpu(DadosTelemetriaVeiculo& dados) {
    dados.aceleracao_x = dados.aceleracao_y = dados.aceleracao_z = 0;
    dados.giroscopio_x = dados.giroscopio_y = dados.giroscopio_z = 0;
    dados.aceleracao_x_g = dados.aceleracao_y_g = dados.aceleracao_z_g = 0.0f;
    dados.aceleracao_x_ms2 = dados.aceleracao_y_ms2 = dados.aceleracao_z_ms2 = 0.0f;
    dados.giroscopio_x_graus_s = dados.giroscopio_y_graus_s =
        dados.giroscopio_z_graus_s = 0.0f;
    dados.temperatura_mpu_c = 0.0f;
    dados.mpu_ativo = dados.mpu_calibrado =
        dados.eixo_longitudinal_compativel = false;
    dados.eixo_longitudinal_mpu = 0;
    dados.sentido_aceleracao_longitudinal = 0;
    dados.aceleracao_g = dados.aceleracao_ms2 = 0.0f;
}

esp_err_t iniciar_mpu6050() {
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
    const esp_err_t erro_mpu = servico_aceleracao_iniciar(configuracao_aceleracao);
    aquisicao.mpu6050 = erro_mpu == ESP_OK;
    (void)registrar_resultado_sensor(
        SensorControlado::Mpu6050, erro_mpu);
    return erro_mpu;
}

void tarefa_telemetria(void*) {
    // A aquisição só toca os dispositivos depois de associada à supervisão.
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uint16_t tensao_bruta = 0;
    DadosTelemetriaVeiculo ultimos_dados{};
    int64_t ultimo_log_erro_envio_ms = -5000;
    int64_t ultimo_log_erro_odometro_ms = -5000;
    int64_t proxima_tentativa_ina228_ms = aquisicao.ina228
        ? 0 : esp_timer_get_time() / 1000 + 5000;
    int64_t proxima_tentativa_mpu_ms =
        esp_timer_get_time() / 1000 + configuracao::INTERVALO_RECUPERACAO_MPU_MS;
    while (true) {
        servico_supervisao_alimentar(ModuloSupervisionado::TarefaTelemetria);
        const bool usar_simulador = servico_simulador_telemetria_esta_ativo();
        if (ultimos_dados.simulado != usar_simulador) ultimos_dados = {};
        if (usar_simulador) {
            const esp_err_t erro_simulador =
                servico_simulador_telemetria_gerar(&ultimos_dados);
            if (erro_simulador != ESP_OK) {
                ESP_LOGE(ETIQUETA, "Falha no simulador de telemetria: %s",
                         esp_err_to_name(erro_simulador));
            }
        } else {
            const uint32_t sensores_ativos = obter_mascara_sensores();
            const auto sensor_ativo = [sensores_ativos](SensorControlado sensor) {
                return (sensores_ativos & bit_sensor(sensor)) != 0;
            };
            const int64_t instante_ina_ms = esp_timer_get_time() / 1000;
            if (sensor_ativo(SensorControlado::Ina228) && !aquisicao.ina228 &&
                instante_ina_ms >= proxima_tentativa_ina228_ms) {
                const esp_err_t erro = ina228_iniciar();
                aquisicao.ina228 = erro == ESP_OK;
                proxima_tentativa_ina228_ms = instante_ina_ms + 5000;
                (void)registrar_resultado_sensor(
                    SensorControlado::Ina228, erro);
                if (erro == ESP_OK) ESP_LOGI(ETIQUETA, "INA228 recuperado");
            }
            LeituraIna228 leitura_ina{};
            const esp_err_t erro_ina = sensor_ativo(SensorControlado::Ina228) && aquisicao.ina228
                ? ina228_ler(&leitura_ina) : ESP_ERR_INVALID_STATE;
            if (sensor_ativo(SensorControlado::Ina228) && aquisicao.ina228) {
                (void)registrar_resultado_sensor(
                    SensorControlado::Ina228, erro_ina);
                if (erro_ina == ESP_OK) {
                    ultimos_dados.tensao_pacote_v = leitura_ina.tensao_barramento_v;
                    ultimos_dados.corrente_a = leitura_ina.corrente_a;
                    ultimos_dados.potencia_w = leitura_ina.potencia_w;
                    ultimos_dados.eletrica_valida = true;
                    ultimos_dados.ina_tensao_shunt_mv = leitura_ina.tensao_shunt_mv;
                    ultimos_dados.ina_temperatura_c = leitura_ina.temperatura_chip_c;
                    ultimos_dados.ina_energia_wh = leitura_ina.energia_j / 3600.0;
                    ultimos_dados.ina_carga_ah = leitura_ina.carga_c / 3600.0;
                    ultimos_dados.ina_diagnostico = leitura_ina.diagnostico;
                    ultimos_dados.ina_energia_valida = leitura_ina.energia_valida;
                    ultimos_dados.ina_carga_valida = leitura_ina.carga_valida;
                } else if (ina228_obter_estado().erros_consecutivos >= 3) {
                    aquisicao.ina228 = false;
                    proxima_tentativa_ina228_ms = instante_ina_ms + 5000;
                    ESP_LOGW(ETIQUETA, "INA228 indisponível; nova tentativa em 5 s: %s",
                             esp_err_to_name(erro_ina));
                }
            }
            if (erro_ina != ESP_OK) {
                ultimos_dados.tensao_pacote_v = 0.0f;
                ultimos_dados.corrente_a = 0.0f;
                ultimos_dados.potencia_w = 0.0f;
                ultimos_dados.eletrica_valida = false;
                ultimos_dados.ina_tensao_shunt_mv = 0.0f;
                ultimos_dados.ina_temperatura_c = 0.0f;
                ultimos_dados.ina_energia_wh = 0.0;
                ultimos_dados.ina_carga_ah = 0.0;
                ultimos_dados.ina_diagnostico = 0;
                ultimos_dados.ina_energia_valida = false;
                ultimos_dados.ina_carga_valida = false;
            }
            if (sensor_ativo(SensorControlado::Adc) && aquisicao.adc) {
                const esp_err_t erro_adc = leitor_tensao_ler(&tensao_bruta);
                (void)registrar_resultado_sensor(
                    SensorControlado::Adc, erro_adc);
                if (erro_adc == ESP_OK) {
                    ultimos_dados.tensao_adc_bruta = tensao_bruta;
                }
            } else {
                ultimos_dados.tensao_adc_bruta = 0;
            }
            if (sensor_ativo(SensorControlado::Mpu6050) && !aquisicao.mpu6050 &&
                instante_ina_ms >= proxima_tentativa_mpu_ms) {
                const esp_err_t erro = iniciar_mpu6050();
                proxima_tentativa_mpu_ms =
                    esp_timer_get_time() / 1000 + configuracao::INTERVALO_RECUPERACAO_MPU_MS;
                if (erro == ESP_OK) ESP_LOGI(ETIQUETA, "Amostragem do MPU6050 recuperada");
            }
            if (sensor_ativo(SensorControlado::Mpu6050) && aquisicao.mpu6050) {
                const EstadoServicoAceleracao estado_aceleracao =
                    servico_aceleracao_obter_estado();
                const esp_err_t erro_mpu = estado_aceleracao.ultimo_erro;
                (void)registrar_resultado_sensor(
                    SensorControlado::Mpu6050, erro_mpu);
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
                    limpar_leitura_mpu(ultimos_dados);
                }
            } else {
                limpar_leitura_mpu(ultimos_dados);
            }
            if (sensor_ativo(SensorControlado::Encoders) && aquisicao.odometro) {
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
            } else {
                ultimos_dados.velocidade_kmh = 0.0f;
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
        consumir_amostra_local(ultimos_dados);
        portENTER_CRITICAL(&trava_amostra);
        ultima_amostra = ultimos_dados;
        amostra_disponivel = true;
        portEXIT_CRITICAL(&trava_amostra);
        ulTaskNotifyTake(
            pdTRUE,
            pdMS_TO_TICKS(servico_telemetria_obter_intervalo_atual()));
    }
}

esp_err_t criar_tarefa_telemetria() {
    if (tarefa_telemetria_handle != nullptr) return ESP_ERR_INVALID_STATE;
    if (configuracao::criar_tarefa(tarefa_telemetria, "telemetria", nullptr, &tarefa_telemetria_handle) != pdPASS) {
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
            xTaskNotifyGive(tarefa_telemetria_handle);
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

}  // namespace

esp_err_t servico_aquisicao_preparar(RegistradorResultadoSensor registrar_resultado) {
    if (registrar_resultado == nullptr) return ESP_ERR_INVALID_ARG;
    const esp_err_t erro = servico_simulador_telemetria_iniciar(
        configuracao::DINAMICA_SIMULACAO_PERCENTUAL);
    if (erro != ESP_OK) return erro;
    registrar_resultado_sensor = registrar_resultado;
    const esp_err_t erro_adc = leitor_tensao_iniciar();
    aquisicao.adc = erro_adc == ESP_OK;
    (void)registrar_resultado_sensor(
        SensorControlado::Adc, erro_adc);
    if (erro_adc != ESP_OK) {
        ESP_LOGW(ETIQUETA, "ADC indisponível; telemetria seguirá degradada: %s",
                 esp_err_to_name(erro_adc));
    }

    const esp_err_t erro_ina = ina228_iniciar();
    aquisicao.ina228 = erro_ina == ESP_OK;
    (void)registrar_resultado_sensor(
        SensorControlado::Ina228, erro_ina);
    if (erro_ina != ESP_OK) {
        ESP_LOGW(ETIQUETA, "INA228 indisponível; leituras elétricas suspensas: %s",
                 esp_err_to_name(erro_ina));
    }

    const esp_err_t erro_mpu = iniciar_mpu6050();
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
    (void)registrar_resultado_sensor(
        SensorControlado::Encoders, erro_encoder);
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
    (void)registrar_resultado_sensor(
        SensorControlado::Encoders, erro_odometro);
    if (erro_odometro != ESP_OK) {
        ESP_LOGW(ETIQUETA, "Odômetro indisponível; telemetria seguirá degradada: %s",
                 esp_err_to_name(erro_odometro));
    }
    return ESP_OK;
}

esp_err_t servico_aquisicao_iniciar(ConsumidorAmostraLocal consumidor) {
    if (consumidor == nullptr) return ESP_ERR_INVALID_ARG;
    if (registrar_resultado_sensor == nullptr) return ESP_ERR_INVALID_STATE;
    esp_err_t erro = criar_tarefa_telemetria();
    if (erro != ESP_OK) return erro;

    consumir_amostra_local = consumidor;
    const auto modulo = ModuloSupervisionado::TarefaTelemetria;
    erro = servico_supervisao_registrar(
        modulo, "tarefa de telemetria",
        configuracao::INTERVALO_TELEMETRIA_OTA_MAXIMO_MS +
            configuracao::MARGEM_SUPERVISAO_TELEMETRIA_MS,
        recuperar_tarefa_telemetria);
    if (erro == ESP_OK) {
        erro = servico_supervisao_associar_tarefa(modulo, tarefa_telemetria_handle);
        if (erro != ESP_OK) (void)servico_supervisao_desregistrar(modulo);
    }
    if (erro != ESP_OK) {
        vTaskDelete(tarefa_telemetria_handle);
        tarefa_telemetria_handle = nullptr;
        return erro;
    }
    xTaskNotifyGive(tarefa_telemetria_handle);
    return ESP_OK;
}

bool servico_aquisicao_obter_ultima_amostra(DadosTelemetriaVeiculo& dados) {
    portENTER_CRITICAL(&trava_amostra);
    dados = ultima_amostra;
    const bool disponivel = amostra_disponivel;
    portEXIT_CRITICAL(&trava_amostra);
    return disponivel;
}

bool servico_aquisicao_sensor_ativo(SensorControlado sensor) {
    return (obter_mascara_sensores() & bit_sensor(sensor)) != 0;
}

uint32_t servico_aquisicao_obter_sensores_ativos() {
    return obter_mascara_sensores();
}

void servico_aquisicao_definir_sensor(SensorControlado sensor, bool ativo) {
    const uint32_t bit = bit_sensor(sensor);
    portENTER_CRITICAL(&trava_sensores);
    if (ativo) sensores_ativos |= bit;
    else sensores_ativos &= ~bit;
    portEXIT_CRITICAL(&trava_sensores);
}

void servico_aquisicao_definir_todos_sensores(bool ativos) {
    portENTER_CRITICAL(&trava_sensores);
    sensores_ativos = ativos ? TODOS : 0u;
    portEXIT_CRITICAL(&trava_sensores);
}
