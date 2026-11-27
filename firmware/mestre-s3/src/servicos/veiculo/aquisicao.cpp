#include "servicos/veiculo/aquisicao.h"
#include "servicos/manutencao/ajustes.h"

#include <algorithm>

#include "dispositivos/entradas/encoder.h"
#include "dispositivos/entradas/ina228.h"
#include "dispositivos/entradas/adc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nucleo/configuracao/placa.h"
#include "nucleo/configuracao/tarefas.h"
#include "nucleo/configuracao/diagnostico.h"
#include "nucleo/configuracao/veiculo.h"
#include "nucleo/tempo.h"
#include "servicos/veiculo/aceleracao.h"
#include "servicos/veiculo/controle.h"
#include "servicos/veiculo/odometro.h"

namespace {
constexpr char ETIQUETA[] = "mestre";
TaskHandle_t tarefa_sensores_handle = nullptr;

struct DisponibilidadeAquisicao {
    bool adc = false;
    bool ina228 = false;
    bool mpu6050 = false;
};

DisponibilidadeAquisicao aquisicao{};

constexpr uint32_t bit_sensor(SensorControlado sensor) {
    return 1u << static_cast<uint8_t>(sensor);
}

constexpr uint32_t TODOS = bit_sensor(SensorControlado::Quantidade) - 1u;
uint32_t sensores_ativos = TODOS;
uint32_t geracao_sensores = 0;

struct AmostrasEletricas {
    LeituraIna228 ina{};
    uint16_t tensao_adc = 0;
    int64_t instante_ina_us = 0;
    int64_t instante_adc_us = 0;
};

AmostrasEletricas amostras_eletricas{};
float ultimo_soc_percentual = -1.0f;
portMUX_TYPE trava_sensores = portMUX_INITIALIZER_UNLOCKED;
RegistradorResultadoSensor registrar_resultado_sensor = nullptr;

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

void aplicar_leitura_ina(DadosTelemetriaVeiculo& dados,
                         const LeituraIna228& leitura) {
    const auto ajustes = ajustes_desenvolvedor_obter();
    const float tensao = leitura.tensao_barramento_v * ajustes.fator_tensao_ina228;
    const float corrente = leitura.corrente_a * ajustes.fator_corrente_ina228;
    // Limites do quadro UART. Um ganho que exceda a faixa invalida somente
    // a leitura elétrica publicada. A amostragem I2C continua normalmente.
    if (!(tensao >= 0.0f && tensao <= 65.535f &&
          corrente >= -327.68f && corrente <= 327.67f)) return;

    dados.tensao_pacote_v = tensao;
    dados.corrente_a = corrente;
    dados.potencia_w = tensao * corrente;
    dados.eletrica_valida = true;
    dados.ina_tensao_shunt_mv = leitura.tensao_shunt_mv;
    dados.ina_temperatura_c = leitura.temperatura_chip_c;
    dados.ina_energia_wh = leitura.energia_j / 3600.0 *
        ajustes.fator_tensao_ina228 * ajustes.fator_corrente_ina228;
    dados.ina_carga_ah = leitura.carga_c / 3600.0 * ajustes.fator_corrente_ina228;
    dados.ina_diagnostico = leitura.diagnostico;
    dados.ina_energia_valida = leitura.energia_valida;
    dados.ina_carga_valida = leitura.carga_valida;
}

esp_err_t iniciar_mpu6050() {
    ConfiguracaoServicoAceleracao configuracao_aceleracao{};
    configuracao_aceleracao.eixo_longitudinal =
        static_cast<EixoAceleracaoVeiculo>(
            configuracao::EIXO_LONGITUDINAL_MPU6050);
    configuracao_aceleracao.sinal_eixo_longitudinal =
        configuracao::SINAL_EIXO_LONGITUDINAL_MPU6050;
    configuracao_aceleracao.frequencia_corte_filtro_hz =
        configuracao::FREQUENCIA_CORTE_ACELERACAO_HZ;
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

void tarefa_sensores(void*) {
    const TickType_t periodo = pdMS_TO_TICKS(configuracao::PERIODO_SENSORES_ELETRICOS_MS);
    configASSERT(periodo > 0);
    TickType_t ultimo_ciclo = xTaskGetTickCount();
    int64_t proxima_ina_ms = aquisicao.ina228 ? 0 : tempo_monotonico::agora_ms() + 5000;
    int64_t proximo_mpu_ms = tempo_monotonico::agora_ms() + configuracao::INTERVALO_RECUPERACAO_MPU_MS;

    while (true) {
        portENTER_CRITICAL(&trava_sensores);
        const uint32_t mascara = sensores_ativos;
        const uint32_t geracao = geracao_sensores;
        portEXIT_CRITICAL(&trava_sensores);
        const int64_t agora_ms = tempo_monotonico::agora_ms();
        AmostrasEletricas amostras{};

        if (mascara & bit_sensor(SensorControlado::Ina228)) {
            if (aquisicao.ina228) {
                const esp_err_t erro = ina228_ler(&amostras.ina);
                (void)registrar_resultado_sensor(SensorControlado::Ina228, erro);
                if (erro == ESP_OK) {
                    amostras.instante_ina_us = esp_timer_get_time();
                } else if (ina228_obter_estado().erros_consecutivos >= 3) {
                    aquisicao.ina228 = false;
                    proxima_ina_ms = tempo_monotonico::agora_ms() + 5000;
                    ESP_LOGW(ETIQUETA, "INA228 indisponível; nova tentativa em 5 s: %s",
                             esp_err_to_name(erro));
                }
            } else if (agora_ms >= proxima_ina_ms) {
                const esp_err_t erro = ina228_iniciar();
                aquisicao.ina228 = erro == ESP_OK;
                proxima_ina_ms = agora_ms + 5000;
                (void)registrar_resultado_sensor(SensorControlado::Ina228, erro);
                // Aguarda o ciclo seguinte para a primeira conversão ficar pronta.
                if (aquisicao.ina228) ESP_LOGI(ETIQUETA, "INA228 recuperado");
            }
        }
        if ((mascara & bit_sensor(SensorControlado::Adc)) && aquisicao.adc) {
            const esp_err_t erro = leitor_pedal_obter_bruto(&amostras.tensao_adc);
            (void)registrar_resultado_sensor(SensorControlado::Adc, erro);
            if (erro == ESP_OK) amostras.instante_adc_us = esp_timer_get_time();
        }
        float soc = -1.0f;
        if (amostras.instante_ina_us > 0) {
            const auto rodas = servico_odometro_obter_estado();
            const auto ajustes = ajustes_desenvolvedor_obter();
            if (servico_odometro_velocidade_valida(rodas, esp_timer_get_time()) &&
                rodas.parado_desde_us > 0 && amostras.instante_ina_us - rodas.parado_desde_us >=
                    ajustes.soc_repouso_ms * 1000LL) {
                const float tensao_mv = amostras.ina.tensao_barramento_v *
                    ajustes.fator_tensao_ina228 * 1000.0f;
                const float vazio_mv = ajustes.soc_baterias_serie * ajustes.soc_vazio_mv;
                const float cheio_mv = ajustes.soc_baterias_serie * ajustes.soc_cheio_mv;
                if (tensao_mv > 0.0f && tensao_mv <= UINT16_MAX)
                    soc = std::clamp((tensao_mv - vazio_mv) * 100.0f / (cheio_mv - vazio_mv),
                                     0.0f, 100.0f);
            }
        }
        portENTER_CRITICAL(&trava_sensores);
        if (geracao_sensores == geracao) {
            amostras_eletricas = amostras;
            if (soc >= 0.0f) ultimo_soc_percentual = soc;
        }
        portEXIT_CRITICAL(&trava_sensores);

        if (mascara & bit_sensor(SensorControlado::Mpu6050)) {
            if (!aquisicao.mpu6050 && agora_ms >= proximo_mpu_ms) {
                const esp_err_t erro = iniciar_mpu6050();
                proximo_mpu_ms = tempo_monotonico::agora_ms() + configuracao::INTERVALO_RECUPERACAO_MPU_MS;
                if (erro == ESP_OK) ESP_LOGI(ETIQUETA, "Amostragem do MPU6050 recuperada");
            }
            if (aquisicao.mpu6050)
                (void)registrar_resultado_sensor(SensorControlado::Mpu6050,
                                               servico_aceleracao_obter_estado().ultimo_erro);
        }
        servico_odometro_persistir_pendente();
        if (xTaskDelayUntil(&ultimo_ciclo, periodo) == pdFALSE)
            ultimo_ciclo = xTaskGetTickCount();
    }
}

esp_err_t iniciar_tarefa_sensores() {
    if (tarefa_sensores_handle != nullptr) return ESP_OK;
    if (configuracao::criar_tarefa(tarefa_sensores, "sensores", nullptr,
                                   &tarefa_sensores_handle) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

}  // namespace

DadosTelemetriaVeiculo servico_aquisicao_obter_amostra_real() {
    DadosTelemetriaVeiculo dados{};
    portENTER_CRITICAL(&trava_sensores);
    const AmostrasEletricas eletricas = amostras_eletricas;
    const float soc = ultimo_soc_percentual;
    const uint32_t mascara = sensores_ativos;
    portEXIT_CRITICAL(&trava_sensores);
    dados.soc_estimado = soc >= 0.0f;
    if (dados.soc_estimado) dados.carga_percentual = soc;
    const auto rodas = servico_odometro_obter_estado();
    const int64_t agora_us = esp_timer_get_time();
    const auto recente = [agora_us](int64_t instante_us) {
        return instante_us > 0 && agora_us - instante_us <= configuracao::PRAZO_SENSORES_ELETRICOS_MS * 1000LL;
    };
    if (recente(eletricas.instante_ina_us))
        aplicar_leitura_ina(dados, eletricas.ina);
    if (recente(eletricas.instante_adc_us)) dados.tensao_adc_bruta = eletricas.tensao_adc;

    if (mascara & bit_sensor(SensorControlado::Mpu6050)) {
        const auto aceleracao = servico_aceleracao_obter_estado();
        dados.mpu_ativo = aceleracao.iniciado && aceleracao.ultimo_erro == ESP_OK;
        dados.mpu_calibrado = aceleracao.calibrado;
        dados.eixo_longitudinal_compativel = aceleracao.eixo_longitudinal_compativel;
        if (dados.mpu_ativo && aceleracao.leituras_ok > 0)
            aplicar_leitura_mpu(dados, aceleracao);
    }

    dados.percurso_km = static_cast<float>(rodas.percurso_m / 1000.0);
    dados.odometro_km = static_cast<float>(rodas.odometro_m / 1000.0);
    if (servico_odometro_velocidade_valida(rodas, agora_us))
        dados.velocidade_kmh = rodas.velocidade_kmh;
    const auto saida = servico_controle_remoto_obter_saida_aplicada();
    dados.acelerador_percentual = saida.acelerador_decimos / 10.0f;
    if (saida.rele_energizado)
        dados.marcha = saida.reversao ? MarchaVeiculo::Re : MarchaVeiculo::Frente;
    return dados;
}

esp_err_t servico_aquisicao_preparar(RegistradorResultadoSensor registrar_resultado) {
    if (registrar_resultado == nullptr) return ESP_ERR_INVALID_ARG;
    registrar_resultado_sensor = registrar_resultado;
    const esp_err_t erro_adc = leitor_pedal_iniciar();
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
    const esp_err_t erro_encoder_esquerdo = configuracao::USAR_ENCODER_ESQUERDO
        ? encoder_incremental_iniciar(EncoderIncrementalId::RodaEsquerda, encoder_esquerdo)
        : ESP_OK;

    ConfiguracaoEncoderIncremental encoder_direito{};
    encoder_direito.pino_a = configuracao::PINO_ENCODER_DIREITO_A;
    encoder_direito.pino_b = configuracao::PINO_ENCODER_DIREITO_B;
    encoder_direito.pulsos_por_giro =
        configuracao::PULSOS_ENCODER_DIREITO_POR_GIRO;
    encoder_direito.filtro_ruido_ns = configuracao::FILTRO_RUIDO_ENCODER_NS;
    const esp_err_t erro_encoder_direito = configuracao::USAR_ENCODER_DIREITO
        ? encoder_incremental_iniciar(EncoderIncrementalId::RodaDireita, encoder_direito)
        : ESP_OK;
    const esp_err_t erro_encoder = erro_encoder_esquerdo != ESP_OK
                                       ? erro_encoder_esquerdo
                                       : erro_encoder_direito;
    (void)registrar_resultado_sensor(
        SensorControlado::Encoders, erro_encoder);
    if (erro_encoder != ESP_OK) {
        ESP_LOGW(ETIQUETA,
                 "Odômetro indisponível; falha nos encoders configurados (%s/%s)",
                 esp_err_to_name(erro_encoder_esquerdo),
                 esp_err_to_name(erro_encoder_direito));
        return iniciar_tarefa_sensores();
    }

    ConfiguracaoOdometro configuracao_odometro{};
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
    (void)registrar_resultado_sensor(
        SensorControlado::Encoders, erro_odometro);
    if (erro_odometro != ESP_OK) {
        ESP_LOGW(ETIQUETA, "Odômetro indisponível; telemetria seguirá degradada: %s",
                 esp_err_to_name(erro_odometro));
    }
    return iniciar_tarefa_sensores();
}

bool servico_aquisicao_sensor_ativo(SensorControlado sensor) {
    return (obter_mascara_sensores() & bit_sensor(sensor)) != 0;
}

uint32_t servico_aquisicao_obter_sensores_ativos() {
    return obter_mascara_sensores();
}

namespace {
void definir_mascara_sensores(uint32_t mascara) {
    const uint32_t alterados = sensores_ativos ^ mascara;
    if (alterados == 0) return;
    sensores_ativos = mascara;
    ++geracao_sensores;
    if (alterados & bit_sensor(SensorControlado::Ina228))
        amostras_eletricas.instante_ina_us = 0;
    if (alterados & bit_sensor(SensorControlado::Adc))
        amostras_eletricas.instante_adc_us = 0;
    if (alterados & bit_sensor(SensorControlado::Encoders))
        servico_odometro_definir_ativo((mascara & bit_sensor(SensorControlado::Encoders)) != 0);
}
}  // namespace

void servico_aquisicao_definir_sensor(SensorControlado sensor, bool ativo) {
    const uint32_t bit = bit_sensor(sensor);
    portENTER_CRITICAL(&trava_sensores);
    definir_mascara_sensores(ativo ? sensores_ativos | bit : sensores_ativos & ~bit);
    portEXIT_CRITICAL(&trava_sensores);
}

void servico_aquisicao_definir_todos_sensores(bool ativos) {
    portENTER_CRITICAL(&trava_sensores);
    definir_mascara_sensores(ativos ? TODOS : 0u);
    portEXIT_CRITICAL(&trava_sensores);
}
