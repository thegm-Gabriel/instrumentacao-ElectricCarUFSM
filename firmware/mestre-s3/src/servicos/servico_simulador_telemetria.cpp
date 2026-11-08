// Gera dados veiculares simulados para validação sem sensores.
#include "servicos/servico_simulador_telemetria.h"
#include "nucleo/configuracao_placa.h"

#include <algorithm>
#include <cstddef>
#include <cmath>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

namespace {
constexpr char ETIQUETA[] = "simulador_telemetria";
constexpr float PI = 3.14159265358979323846f;
constexpr uint16_t DINAMICA_MINIMA = 25;
constexpr uint16_t DINAMICA_MAXIMA = 200;

struct EstadoSimuladorTelemetria {
    bool iniciado = false;
    bool ativo = false;
    uint32_t amostras_geradas = 0;
    uint16_t dinamica_percentual = 100;
    uint32_t semente = 0;
    float tempo_simulado_s = 0.0f;
    float percurso_km = 0.0f;
};

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

uint32_t misturar(uint32_t valor) {
    valor ^= valor >> 16;
    valor *= 0x7feb352dU;
    valor ^= valor >> 15;
    valor *= 0x846ca68bU;
    return valor ^ (valor >> 16);
}

float sortear(uint32_t semente, uint32_t episodio, uint32_t canal) {
    const uint32_t bits = misturar(semente ^ (episodio * 0x9e3779b9U) ^
                                   (canal * 0x85ebca6bU));
    return static_cast<float>(bits & 0xffffffU) / 16777216.0f;
}

MovimentoSimulado gerar_movimento(float segundos, uint32_t semente) {
    constexpr float CICLO_S = 45.0f;
    constexpr size_t TRECHOS = 6;
    const uint32_t episodio = static_cast<uint32_t>(segundos / CICLO_S);
    const float ciclo = std::fmod(segundos, CICLO_S);
    const bool re = episodio != 0 && sortear(semente, episodio, 7) < 0.14f;
    const float limite = re ? 9.0f : 38.0f;
    const float minimo = re ? 2.0f : 5.0f;
    const float pausa = 1.0f + 2.0f * sortear(semente, episodio, 2);
    const float pausa_final = 1.0f + 3.0f * sortear(semente, episodio, 4);
    const float duracao_ativa = CICLO_S - pausa - pausa_final;
    float alvos[TRECHOS + 1] = {};
    float pesos[TRECHOS] = {};
    float soma_pesos = 0.0f;
    alvos[1] = re ? 3.0f + 5.0f * sortear(semente, episodio, 1)
                  : 9.0f + 19.0f * sortear(semente, episodio, 1);
    for (size_t i = 2; i < TRECHOS; ++i) {
        const float amplitude = re ? 7.0f : (i == 2 ? 16.0f : 23.0f);
        alvos[i] = std::clamp(alvos[i - 1] +
                                  (sortear(semente, episodio, 10 + i) - 0.5f) * amplitude,
                              minimo, limite);
    }
    if (episodio == 0 && !re) alvos[5] = std::max(alvos[5], 20.0f);
    for (size_t i = 0; i < TRECHOS; ++i) {
        // Pesos distintos criam subidas, descidas e trechos quase constantes.
        pesos[i] = i == TRECHOS - 1 && episodio == 0
                       ? 0.45f + 0.2f * sortear(semente, episodio, 20 + i)
                       : 0.6f + sortear(semente, episodio, 20 + i);
        soma_pesos += pesos[i];
    }
    const MarchaVeiculo marcha = re ? MarchaVeiculo::Re : MarchaVeiculo::Frente;
    MovimentoSimulado movimento{MarchaVeiculo::Estacionado, 0, 0, 0, 0};
    if (ciclo < pausa) return movimento;
    float decorrido = ciclo - pausa;
    for (size_t i = 0; i < TRECHOS; ++i) {
        const float duracao = duracao_ativa * pesos[i] / soma_pesos;
        if (decorrido < duracao) {
            const float fase = std::clamp(decorrido / duracao, 0.0f, 1.0f);
            const float variacao = alvos[i + 1] - alvos[i];
            const float suavizacao = fase * fase * (3.0f - 2.0f * fase);
            movimento.marcha = marcha;
            movimento.velocidade_kmh = alvos[i] + variacao * suavizacao;
            movimento.aceleracao_ms2 = variacao * 6.0f * fase * (1.0f - fase) /
                                       duracao / 3.6f;
            return movimento;
        }
        decorrido -= duracao;
    }
    return {MarchaVeiculo::Estacionado, 0, 0, 0, 0};
}

void atualizar_pedais(MovimentoSimulado* movimento) {
    if (movimento->aceleracao_ms2 > 0.03f) {
        movimento->acelerador_percentual = std::clamp(
            11.0f + movimento->velocidade_kmh * 0.25f +
                movimento->aceleracao_ms2 * 28.0f,
            0.0f, 100.0f);
    } else if (movimento->aceleracao_ms2 < -0.16f) {
        movimento->freio_percentual = std::clamp(
            8.0f - movimento->aceleracao_ms2 * 30.0f, 0.0f, 100.0f);
    } else if (movimento->aceleracao_ms2 >= -0.03f && movimento->velocidade_kmh > 1.0f) {
        movimento->acelerador_percentual = 8.0f + movimento->velocidade_kmh * 0.42f;
    }
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
    const uint32_t semente = esp_random();
    portENTER_CRITICAL(&trava_estado);
    estado = {};
    estado.iniciado = true;
    estado.ativo = configuracao::USAR_TELEMETRIA_SIMULADA;
    estado.dinamica_percentual = dinamica_percentual;
    estado.semente = semente;
    instante_anterior_us = esp_timer_get_time();
    portEXIT_CRITICAL(&trava_estado);
    ESP_LOGW(ETIQUETA, "Gerador simulado pronto; estado inicial: %s; semente: %lu",
             configuracao::USAR_TELEMETRIA_SIMULADA ? "ativo" : "inativo",
             static_cast<unsigned long>(semente));
    return ESP_OK;
}

esp_err_t servico_simulador_telemetria_definir_ativo(bool ativo) {
    portENTER_CRITICAL(&trava_estado);
    if (!estado.iniciado) {
        portEXIT_CRITICAL(&trava_estado);
        return ESP_ERR_INVALID_STATE;
    }
    const bool mudou = estado.ativo != ativo;
    if (mudou) {
        estado.ativo = ativo;
        if (ativo) instante_anterior_us = esp_timer_get_time();
    }
    portEXIT_CRITICAL(&trava_estado);
    if (mudou) ESP_LOGW(ETIQUETA, "Telemetria simulada %s por comando; voltará ao padrão após reiniciar",
                        ativo ? "ativada" : "desativada");
    return ESP_OK;
}

bool servico_simulador_telemetria_esta_ativo() {
    portENTER_CRITICAL(&trava_estado);
    const bool ativo = estado.ativo;
    portEXIT_CRITICAL(&trava_estado);
    return ativo;
}

uint32_t servico_simulador_telemetria_amostras_geradas() {
    portENTER_CRITICAL(&trava_estado);
    const uint32_t amostras = estado.amostras_geradas;
    portEXIT_CRITICAL(&trava_estado);
    return amostras;
}

uint16_t servico_simulador_telemetria_dinamica_percentual() {
    portENTER_CRITICAL(&trava_estado);
    const uint16_t dinamica = estado.dinamica_percentual;
    portEXIT_CRITICAL(&trava_estado);
    return dinamica;
}

esp_err_t servico_simulador_telemetria_gerar(DadosTelemetriaVeiculo* dados) {
    if (dados == nullptr) return ESP_ERR_INVALID_ARG;

    portENTER_CRITICAL(&trava_estado);
    if (!estado.iniciado || !estado.ativo) {
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
    const uint32_t semente = estado.semente;
    portEXIT_CRITICAL(&trava_estado);

    MovimentoSimulado movimento = gerar_movimento(tempo_s, semente);
    movimento.aceleracao_ms2 *= fator_dinamica;
    atualizar_pedais(&movimento);

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
    dados->aceleracao_g = dados->aceleracao_ms2 / 9.80665f;
    dados->acelerador_percentual = movimento.acelerador_percentual;
    dados->freio_percentual = movimento.freio_percentual;
    dados->carga_percentual = std::max(35.0f, 88.0f - percurso_km * 0.40f);
    dados->corrente_a = movimento.freio_percentual > 0.0f
                           ? -movimento.freio_percentual * 0.12f *
                                 std::min(1.0f, movimento.velocidade_kmh / 10.0f)
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
    dados->aceleracao_x_g = dados->aceleracao_x / 16384.0f;
    dados->aceleracao_y_g = dados->aceleracao_y / 16384.0f;
    dados->aceleracao_z_g = dados->aceleracao_z / 16384.0f;
    dados->aceleracao_x_ms2 = dados->aceleracao_x_g * 9.80665f;
    dados->aceleracao_y_ms2 = dados->aceleracao_y_g * 9.80665f;
    dados->aceleracao_z_ms2 = dados->aceleracao_z_g * 9.80665f;
    dados->giroscopio_x = limitar_inteiro(80.0f * std::sin(tempo_s / 2.0f));
    dados->giroscopio_y = limitar_inteiro(55.0f * std::cos(tempo_s / 2.8f));
    dados->giroscopio_z = limitar_inteiro(140.0f * std::sin(tempo_s / 4.2f));
    return ESP_OK;
}
