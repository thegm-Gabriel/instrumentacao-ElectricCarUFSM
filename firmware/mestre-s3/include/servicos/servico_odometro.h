// Declara cálculo, validação e persistência do odômetro.
#pragma once

#include <cstdint>

#include "esp_err.h"

struct ConfiguracaoRodaOdometro {
    float diametro_roda_m = 0.0f;
    // Quantidade de giros do encoder para cada giro completo desta roda.
    float giros_encoder_por_giro_roda = 1.0f;
};

struct ConfiguracaoOdometro {
    ConfiguracaoRodaOdometro roda_esquerda{};
    ConfiguracaoRodaOdometro roda_direita{};
    float intervalo_persistencia_m = 100.0f;
    float velocidade_maxima_kmh = 200.0f;
    float razao_minima_movimento_rodas = 0.20f;
    float movimento_minimo_diagnostico_m = 0.05f;
    uint16_t amostras_divergentes_para_alerta = 10;
};

struct EstadoRodaOdometro {
    uint64_t contagens_acumuladas = 0;
    double percurso_m = 0.0;
    float velocidade_kmh = 0.0f;
    double metros_por_contagem = 0.0;
    uint32_t erros_leitura = 0;
    esp_err_t ultimo_erro = ESP_OK;
};

struct EstadoOdometro {
    bool iniciado = false;
    bool persistencia_disponivel = false;
    EstadoRodaOdometro roda_esquerda{};
    EstadoRodaOdometro roda_direita{};
    // Percurso, velocidade e odômetro representam a média das duas rodas.
    double percurso_m = 0.0;
    double odometro_m = 0.0;
    float velocidade_kmh = 0.0f;
    uint32_t atualizacoes = 0;
    uint32_t erros_leitura = 0;
    uint32_t erros_persistencia = 0;
    uint32_t erros_validacao = 0;
    uint32_t divergencias_detectadas = 0;
    uint16_t amostras_divergentes_consecutivas = 0;
    bool rodas_coerentes = true;
    bool persistencia_pendente = false;
    esp_err_t ultimo_erro_leitura = ESP_OK;
    esp_err_t ultimo_erro_validacao = ESP_OK;
    esp_err_t ultimo_erro_persistencia = ESP_OK;
};

esp_err_t servico_odometro_iniciar(const ConfiguracaoOdometro& configuracao);
esp_err_t servico_odometro_atualizar();
EstadoOdometro servico_odometro_obter_estado();
esp_err_t servico_odometro_zerar_percurso();
