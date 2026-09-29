#include "servicos/servico_simulador_telemetria.h"

#include <algorithm>
#include <cstddef>
#include <cmath>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

namespace {
constexpr char ETIQUETA[] = "simulador_telemetria";
constexpr float PI = 3.14159265358979323846f;
constexpr uint16_t DINAMICA_MINIMA = 25;
constexpr uint16_t DINAMICA_MAXIMA = 200;

EstadoSimuladorTelemetria estado{};
int64_t instante_anterior_us = 0;
portMUX_TYPE trava_estado = portMUX_INITIALIZER_UNLOCKED;

struct MovimentoSimulado {
    MarchaVeiculo marcha;
    float velocidade_kmh;
    float aceleracao_ms2;
    float acelerador_percentual;
    float freio_percentual;
};

MovimentoSimulado gerar_movimento(float segundos) {
    const float ciclo = std::fmod(segundos, 80.0f);
    MovimentoSimulado movimento{MarchaVeiculo::Estacionado, 0, 0, 0, 0};

    if (ciclo < 6.0f) {
        movimento.marcha = MarchaVeiculo::Estacionado;
    } else if (ciclo < 10.0f) {
        movimento.marcha = MarchaVeiculo::Neutro;
    } else if (ciclo < 25.0f) {
        const float fase = (ciclo - 10.0f) / 15.0f;
        movimento.marcha = MarchaVeiculo::Frente;
        movimento.velocidade_kmh = 27.5f * (1.0f - std::cos(PI * fase));
        movimento.aceleracao_ms2 =
            (27.5f * PI / 15.0f) * std::sin(PI * fase) / 3.6f;
        movimento.acelerador_percentual = 32.0f + 58.0f * std::sin(PI * fase);
    } else if (ciclo < 42.0f) {
        const float fase = (ciclo - 25.0f) * 2.0f * PI / 17.0f;
        movimento.marcha = MarchaVeiculo::Frente;
        movimento.velocidade_kmh = 55.0f + 4.0f * std::sin(fase);
        movimento.aceleracao_ms2 =
            (4.0f * 2.0f * PI / 17.0f) * std::cos(fase) / 3.6f;
        movimento.acelerador_percentual = 28.0f + 4.0f * std::sin(fase);
    } else if (ciclo < 52.0f) {
        const float fase = (ciclo - 42.0f) / 10.0f;
        movimento.marcha = MarchaVeiculo::Frente;
        movimento.velocidade_kmh = 27.5f * (1.0f + std::cos(PI * fase));
        movimento.aceleracao_ms2 =
            -(27.5f * PI / 10.0f) * std::sin(PI * fase) / 3.6f;
        movimento.freio_percentual = 30.0f + 60.0f * std::sin(PI * fase);
    } else if (ciclo < 65.0f) {
        movimento.marcha = ciclo < 58.0f ? MarchaVeiculo::Neutro
                                         : MarchaVeiculo::Estacionado;
    } else if (ciclo < 72.0f) {
        const float fase = (ciclo - 65.0f) / 7.0f;
        movimento.marcha = MarchaVeiculo::Re;
        movimento.velocidade_kmh = 6.0f * (1.0f - std::cos(PI * fase));
        movimento.aceleracao_ms2 =
            (6.0f * PI / 7.0f) * std::sin(PI * fase) / 3.6f;
        movimento.acelerador_percentual = 24.0f + 34.0f * std::sin(PI * fase);
    } else if (ciclo < 78.0f) {
        const float fase = (ciclo - 72.0f) / 6.0f;
        movimento.marcha = MarchaVeiculo::Re;
        movimento.velocidade_kmh = 6.0f * (1.0f + std::cos(PI * fase));
        movimento.aceleracao_ms2 =
            -(6.0f * PI / 6.0f) * std::sin(PI * fase) / 3.6f;
        movimento.freio_percentual = 28.0f + 52.0f * std::sin(PI * fase);
    } else {
        movimento.marcha = MarchaVeiculo::Neutro;
    }
    return movimento;
}

int16_t limitar_inteiro(float valor) {
    return static_cast<int16_t>(std::clamp(valor, -32768.0f, 32767.0f));
}
}  // namespace

esp_err_t servico_simulador_telemetria_iniciar(uint16_t dinamica_percentual) {
    if (dinamica_percentual < DINAMICA_MINIMA || dinamica_percentual > DINAMICA_MAXIMA) {
        ESP_LOGE(ETIQUETA, "Dinâmica inválida: %u%% (permitido: %u%% a %u%%)",
                 static_cast<unsigned>(dinamica_percentual),
                 static_cast<unsigned>(DINAMICA_MINIMA),
                 static_cast<unsigned>(DINAMICA_MAXIMA));
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&trava_estado);
    estado = {};
    estado.iniciado = true;
    estado.dinamica_percentual = dinamica_percentual;
    instante_anterior_us = esp_timer_get_time();
    portEXIT_CRITICAL(&trava_estado);
    ESP_LOGW(ETIQUETA,
             "Gerador de telemetria simulada ativo; os dados enviados não são medições reais");
    return ESP_OK;
}

esp_err_t servico_simulador_telemetria_gerar(DadosTelemetriaVeiculo* dados) {
    if (dados == nullptr) return ESP_ERR_INVALID_ARG;

    portENTER_CRITICAL(&trava_estado);
    if (!estado.iniciado) {
        portEXIT_CRITICAL(&trava_estado);
        return ESP_ERR_INVALID_STATE;
    }
    const int64_t agora_us = esp_timer_get_time();
    const float delta_s = std::min(static_cast<float>(agora_us - instante_anterior_us) /
                                       1000000.0f,
                                   2.0f);
    instante_anterior_us = agora_us;
    const float fator_dinamica = estado.dinamica_percentual / 100.0f;
    estado.tempo_simulado_s += delta_s * fator_dinamica;
    const float tempo_s = estado.tempo_simulado_s;
    portEXIT_CRITICAL(&trava_estado);

    MovimentoSimulado movimento = gerar_movimento(tempo_s);
    movimento.aceleracao_ms2 *= fator_dinamica;
    movimento.acelerador_percentual =
        std::clamp(movimento.acelerador_percentual * (0.75f + 0.25f * fator_dinamica),
                   0.0f, 100.0f);
    movimento.freio_percentual =
        std::clamp(movimento.freio_percentual * (0.75f + 0.25f * fator_dinamica),
                   0.0f, 100.0f);

    portENTER_CRITICAL(&trava_estado);
    estado.percurso_km += movimento.velocidade_kmh * delta_s / 3600.0f;
    estado.amostras_geradas++;
    const float percurso_km = estado.percurso_km;
    portEXIT_CRITICAL(&trava_estado);

    *dados = {};
    dados->simulado = true;
    dados->marcha = movimento.marcha;
    dados->velocidade_kmh = movimento.velocidade_kmh;
    dados->aceleracao_ms2 = movimento.aceleracao_ms2;
    dados->acelerador_percentual = movimento.acelerador_percentual;
    dados->freio_percentual = movimento.freio_percentual;
    dados->carga_percentual = std::max(35.0f, 88.0f - percurso_km * 0.40f);
    dados->corrente_a = movimento.freio_percentual > 0.0f
                           ? -movimento.freio_percentual * 0.08f
                           : (movimento.acelerador_percentual > 0.0f
                                  ? 2.0f + movimento.acelerador_percentual * 0.28f
                                  : 0.8f);
    dados->tensao_pacote_v = 13.10f - (100.0f - dados->carga_percentual) * 0.006f -
                             dados->corrente_a * 0.004f;
    dados->potencia_w = dados->tensao_pacote_v * dados->corrente_a;
    dados->autonomia_km = dados->carga_percentual * 0.62f;
    dados->percurso_km = percurso_km;
    dados->odometro_km = 1250.0f + percurso_km;
    dados->tensao_adc_bruta = static_cast<uint16_t>(
        std::clamp(dados->tensao_pacote_v / 13.2f * 4095.0f, 0.0f, 4095.0f));

    dados->tensoes_celulas_v[0] = dados->tensao_pacote_v / 4.0f + 0.006f * std::sin(tempo_s / 7.0f);
    dados->tensoes_celulas_v[1] = dados->tensao_pacote_v / 4.0f - 0.004f * std::sin(tempo_s / 9.0f);
    dados->tensoes_celulas_v[2] = dados->tensao_pacote_v / 4.0f + 0.003f * std::sin(tempo_s / 5.0f);
    dados->tensoes_celulas_v[3] = dados->tensao_pacote_v - dados->tensoes_celulas_v[0] -
                                  dados->tensoes_celulas_v[1] - dados->tensoes_celulas_v[2];

    const float angulo_rota = percurso_km * 10.0f;
    dados->latitude = -30.0339 + 0.0045 * std::sin(static_cast<double>(angulo_rota));
    dados->longitude = -52.8931 + 0.0060 * std::cos(static_cast<double>(angulo_rota));
    dados->rumo_graus = std::atan2(-0.0060f * std::sin(angulo_rota),
                                   0.0045f * std::cos(angulo_rota)) * 180.0f / PI;
    if (dados->rumo_graus < 0.0f) dados->rumo_graus += 360.0f;

    constexpr float BASE_DISTANCIAS[8] = {170, 135, 105, 145, 185, 150, 110, 140};
    constexpr float AMPLITUDES[8] = {90, 72, 58, 65, 82, 68, 62, 75};
    constexpr float PERIODOS[8] = {4.3f, 5.1f, 3.7f, 6.2f, 5.7f, 4.8f, 3.9f, 5.4f};
    for (size_t indice = 0; indice < 8; ++indice) {
        dados->distancias_cm[indice] = BASE_DISTANCIAS[indice] +
            AMPLITUDES[indice] * std::sin(tempo_s / PERIODOS[indice] + indice * 0.8f);
    }

    const float escala_acelerometro = 16384.0f / 9.80665f;
    dados->aceleracao_x = limitar_inteiro(dados->aceleracao_ms2 * escala_acelerometro);
    dados->aceleracao_y = limitar_inteiro(280.0f * std::sin(tempo_s / 2.7f));
    dados->aceleracao_z = limitar_inteiro(16384.0f + 180.0f * std::sin(tempo_s / 3.1f));
    dados->giroscopio_x = limitar_inteiro(80.0f * std::sin(tempo_s / 2.0f));
    dados->giroscopio_y = limitar_inteiro(55.0f * std::cos(tempo_s / 2.8f));
    dados->giroscopio_z = limitar_inteiro(140.0f * std::sin(tempo_s / 4.2f));
    return ESP_OK;
}

esp_err_t servico_simulador_telemetria_definir_dinamica(uint16_t percentual) {
    if (percentual < DINAMICA_MINIMA || percentual > DINAMICA_MAXIMA) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&trava_estado);
    if (!estado.iniciado) {
        portEXIT_CRITICAL(&trava_estado);
        return ESP_ERR_INVALID_STATE;
    }
    estado.dinamica_percentual = percentual;
    portEXIT_CRITICAL(&trava_estado);
    ESP_LOGI(ETIQUETA, "Dinâmica da simulação alterada para %u%%",
             static_cast<unsigned>(percentual));
    return ESP_OK;
}

EstadoSimuladorTelemetria servico_simulador_telemetria_obter_estado() {
    portENTER_CRITICAL(&trava_estado);
    const EstadoSimuladorTelemetria copia = estado;
    portEXIT_CRITICAL(&trava_estado);
    return copia;
}
