// Declara aquisição, calibração e filtragem da aceleração veicular.
#pragma once

#include <cstdint>

#include "dispositivos/mpu6050.h"
#include "esp_err.h"

enum class EixoAceleracaoVeiculo : uint8_t {
    X = 0,
    Y,
    Z,
};

// Sentido do vetor de aceleração no eixo longitudinal configurado. Isto não é
// o sentido de deslocamento do veículo: ao frear enquanto avança, por exemplo,
// a aceleração aponta para trás.
enum class SentidoAceleracaoLongitudinal : int8_t {
    Tras = -1,
    Neutro = 0,
    Frente = 1,
};

struct ConfiguracaoServicoAceleracao {
    EixoAceleracaoVeiculo eixo_longitudinal = EixoAceleracaoVeiculo::X;
    float sinal_eixo_longitudinal = 1.0f;
    uint32_t frequencia_amostragem_hz = 100;
    float frequencia_corte_filtro_hz = 3.0f;
    uint16_t amostras_calibracao = 100;
    float limite_giroscopio_calibracao_graus_s = 5.0f;
    float gravidade_minima_calibracao_g = 0.85f;
    float gravidade_maxima_calibracao_g = 1.15f;
    float gravidade_maxima_eixo_longitudinal_g = 0.50f;
    float frequencia_correcao_gravidade_hz = 0.05f;
    float limite_aceleracao_correcao_gravidade_g = 0.012f;
    float limiar_entrada_sentido_g = 0.03f;
    float limiar_saida_sentido_g = 0.015f;
    uint32_t tempo_confirmacao_sentido_ms = 80;
};

struct EstadoServicoAceleracao {
    bool iniciado = false;
    bool calibrado = false;
    LeituraMpu6050 leitura{};
    float aceleracao_longitudinal_g = 0.0f;
    float aceleracao_longitudinal_ms2 = 0.0f;
    float componente_gravidade_longitudinal_g = 0.0f;
    float gravidade_estimada_x_g = 0.0f;
    float gravidade_estimada_y_g = 0.0f;
    float gravidade_estimada_z_g = 0.0f;
    float aceleracao_linear_x_g = 0.0f;
    float aceleracao_linear_y_g = 0.0f;
    float aceleracao_linear_z_g = 0.0f;
    float desvio_giroscopio_x_graus_s = 0.0f;
    float desvio_giroscopio_y_graus_s = 0.0f;
    float desvio_giroscopio_z_graus_s = 0.0f;
    SentidoAceleracaoLongitudinal sentido =
        SentidoAceleracaoLongitudinal::Neutro;
    bool eixo_longitudinal_compativel = false;
    uint16_t amostras_calibracao_coletadas = 0;
    uint32_t leituras_ok = 0;
    uint32_t erros_leitura = 0;
    esp_err_t ultimo_erro = ESP_OK;
};

// Mantém a aquisição em uma tarefa própria para que o filtro receba amostras
// regulares, independentemente do intervalo de publicação da telemetria.
esp_err_t servico_aceleracao_iniciar(
    const ConfiguracaoServicoAceleracao& configuracao);
EstadoServicoAceleracao servico_aceleracao_obter_estado();
