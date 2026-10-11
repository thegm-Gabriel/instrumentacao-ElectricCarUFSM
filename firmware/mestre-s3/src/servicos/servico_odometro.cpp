// Implementa distância, velocidade, coerência das rodas e persistência.
#include "servicos/servico_odometro.h"

#include <cmath>

#include "dispositivos/encoder_incremental.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

namespace {
constexpr char ETIQUETA[] = "servico_odometro";
constexpr char ESPACO_NVS[] = "odometro";
constexpr char CHAVE_ODOMETRO_MM[] = "total_mm";
constexpr double PI = 3.14159265358979323846;
constexpr int64_t INTERVALO_REPETICAO_PERSISTENCIA_US = 30000000;

EstadoOdometro estado{};
ConfiguracaoOdometro parametros{};
portMUX_TYPE trava_estado = portMUX_INITIALIZER_UNLOCKED;
uint64_t contagem_anterior_esquerda = 0;
uint64_t contagem_anterior_direita = 0;
int64_t instante_anterior_us = 0;
double odometro_persistido_m = 0.0;
nvs_handle_t armazenamento = 0;
SemaphoreHandle_t mutex_atualizacao = nullptr;
int64_t proxima_tentativa_persistencia_us = 0;

bool configuracao_roda_valida(const ConfiguracaoRodaOdometro& configuracao) {
    return std::isfinite(configuracao.diametro_roda_m) &&
           configuracao.diametro_roda_m > 0.0f &&
           std::isfinite(configuracao.giros_encoder_por_giro_roda) &&
           configuracao.giros_encoder_por_giro_roda > 0.0f;
}

bool configuracao_valida(const ConfiguracaoOdometro& configuracao) {
    return configuracao_roda_valida(configuracao.roda_esquerda) &&
           configuracao_roda_valida(configuracao.roda_direita) &&
           std::isfinite(configuracao.intervalo_persistencia_m) &&
           configuracao.intervalo_persistencia_m > 0.0f &&
           std::isfinite(configuracao.velocidade_maxima_kmh) &&
           configuracao.velocidade_maxima_kmh > 0.0f &&
           std::isfinite(configuracao.razao_minima_movimento_rodas) &&
           configuracao.razao_minima_movimento_rodas >= 0.0f &&
           configuracao.razao_minima_movimento_rodas <= 1.0f &&
           std::isfinite(configuracao.movimento_minimo_diagnostico_m) &&
           configuracao.movimento_minimo_diagnostico_m >= 0.0f &&
           configuracao.amostras_divergentes_para_alerta > 0;
}

double calcular_metros_por_contagem(
    const ConfiguracaoRodaOdometro& configuracao,
    const EstadoEncoderIncremental& encoder) {
    const double contagens_por_giro_roda =
        static_cast<double>(encoder.contagens_por_giro) *
        configuracao.giros_encoder_por_giro_roda;
    return (PI * configuracao.diametro_roda_m) / contagens_por_giro_roda;
}

double carregar_odometro_m() {
    uint64_t total_mm = 0;
    const esp_err_t erro =
        nvs_get_u64(armazenamento, CHAVE_ODOMETRO_MM, &total_mm);
    if (erro == ESP_OK) return static_cast<double>(total_mm) / 1000.0;
    if (erro != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(ETIQUETA, "Falha ao carregar odômetro: %s",
                 esp_err_to_name(erro));
    }
    return 0.0;
}

esp_err_t persistir_odometro(double odometro_m) {
    if (armazenamento == 0) return ESP_ERR_INVALID_STATE;
    const uint64_t total_mm =
        static_cast<uint64_t>(std::llround(odometro_m * 1000.0));
    esp_err_t erro =
        nvs_set_u64(armazenamento, CHAVE_ODOMETRO_MM, total_mm);
    if (erro == ESP_OK) erro = nvs_commit(armazenamento);
    return erro;
}
}  // namespace

esp_err_t servico_odometro_iniciar(const ConfiguracaoOdometro& configuracao) {
    if (estado.iniciado) return ESP_OK;
    if (!configuracao_valida(configuracao)) return ESP_ERR_INVALID_ARG;
    if (mutex_atualizacao == nullptr) {
        mutex_atualizacao = xSemaphoreCreateMutex();
    }
    if (mutex_atualizacao == nullptr) return ESP_ERR_NO_MEM;

    const EstadoEncoderIncremental encoder_esquerdo =
        encoder_incremental_obter_estado(EncoderIncrementalId::RodaEsquerda);
    const EstadoEncoderIncremental encoder_direito =
        encoder_incremental_obter_estado(EncoderIncrementalId::RodaDireita);
    if (!encoder_esquerdo.inicializado || !encoder_direito.inicializado ||
        encoder_esquerdo.contagens_por_giro == 0 ||
        encoder_direito.contagens_por_giro == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t contagem_inicial_esquerda = 0;
    uint64_t contagem_inicial_direita = 0;
    esp_err_t erro = encoder_incremental_ler(
        EncoderIncrementalId::RodaEsquerda, &contagem_inicial_esquerda);
    if (erro == ESP_OK) {
        erro = encoder_incremental_ler(
            EncoderIncrementalId::RodaDireita, &contagem_inicial_direita);
    }
    if (erro != ESP_OK) return erro;

    bool persistencia_disponivel = false;
    double odometro_inicial_m = 0.0;
    erro = nvs_open(ESPACO_NVS, NVS_READWRITE, &armazenamento);
    if (erro == ESP_OK) {
        persistencia_disponivel = true;
        odometro_inicial_m = carregar_odometro_m();
    } else {
        armazenamento = 0;
        ESP_LOGW(ETIQUETA,
                 "Odômetro seguirá volátil; não foi possível abrir a NVS: %s",
                 esp_err_to_name(erro));
    }

    parametros = configuracao;
    contagem_anterior_esquerda = contagem_inicial_esquerda;
    contagem_anterior_direita = contagem_inicial_direita;
    instante_anterior_us = esp_timer_get_time();
    odometro_persistido_m = odometro_inicial_m;

    portENTER_CRITICAL(&trava_estado);
    estado = {};
    estado.iniciado = true;
    estado.rodas_coerentes = true;
    estado.persistencia_disponivel = persistencia_disponivel;
    estado.odometro_m = odometro_inicial_m;
    estado.ultimo_erro_persistencia =
        persistencia_disponivel ? ESP_OK : erro;
    estado.roda_esquerda.metros_por_contagem =
        calcular_metros_por_contagem(configuracao.roda_esquerda,
                                     encoder_esquerdo);
    estado.roda_direita.metros_por_contagem =
        calcular_metros_por_contagem(configuracao.roda_direita,
                                     encoder_direito);
    portEXIT_CRITICAL(&trava_estado);

    ESP_LOGI(ETIQUETA,
             "Odômetro duplo ativo: resolução esquerda=%.6f m, direita=%.6f m; total=média das rodas",
             estado.roda_esquerda.metros_por_contagem,
             estado.roda_direita.metros_por_contagem);
    return ESP_OK;
}

esp_err_t servico_odometro_atualizar() {
    if (!estado.iniciado) return ESP_ERR_INVALID_STATE;
    if (mutex_atualizacao == nullptr ||
        xSemaphoreTake(mutex_atualizacao, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t contagem_esquerda = 0;
    uint64_t contagem_direita = 0;
    const esp_err_t erro_esquerda = encoder_incremental_ler(
        EncoderIncrementalId::RodaEsquerda, &contagem_esquerda);
    const esp_err_t erro_direita = encoder_incremental_ler(
        EncoderIncrementalId::RodaDireita, &contagem_direita);
    if (erro_esquerda != ESP_OK || erro_direita != ESP_OK) {
        const esp_err_t erro = erro_esquerda != ESP_OK ? erro_esquerda
                                                       : erro_direita;
        portENTER_CRITICAL(&trava_estado);
        if (erro_esquerda != ESP_OK) {
            estado.roda_esquerda.erros_leitura++;
            estado.roda_esquerda.ultimo_erro = erro_esquerda;
        }
        if (erro_direita != ESP_OK) {
            estado.roda_direita.erros_leitura++;
            estado.roda_direita.ultimo_erro = erro_direita;
        }
        estado.erros_leitura++;
        estado.ultimo_erro_leitura = erro;
        portEXIT_CRITICAL(&trava_estado);
        xSemaphoreGive(mutex_atualizacao);
        return erro;
    }

    const int64_t agora_us = esp_timer_get_time();
    const uint64_t novas_contagens_esquerda =
        contagem_esquerda - contagem_anterior_esquerda;
    const uint64_t novas_contagens_direita =
        contagem_direita - contagem_anterior_direita;
    contagem_anterior_esquerda = contagem_esquerda;
    contagem_anterior_direita = contagem_direita;

    const double delta_esquerda_m =
        novas_contagens_esquerda * estado.roda_esquerda.metros_por_contagem;
    const double delta_direita_m =
        novas_contagens_direita * estado.roda_direita.metros_por_contagem;
    const double delta_medio_m = (delta_esquerda_m + delta_direita_m) / 2.0;
    const double delta_s =
        static_cast<double>(agora_us - instante_anterior_us) / 1000000.0;
    instante_anterior_us = agora_us;

    const double velocidade_esquerda_kmh =
        delta_s > 0.0 ? delta_esquerda_m / delta_s * 3.6 : 0.0;
    const double velocidade_direita_kmh =
        delta_s > 0.0 ? delta_direita_m / delta_s * 3.6 : 0.0;
    if (delta_s <= 0.0 ||
        velocidade_esquerda_kmh > parametros.velocidade_maxima_kmh ||
        velocidade_direita_kmh > parametros.velocidade_maxima_kmh) {
        portENTER_CRITICAL(&trava_estado);
        estado.roda_esquerda.velocidade_kmh = 0.0f;
        estado.roda_direita.velocidade_kmh = 0.0f;
        estado.velocidade_kmh = 0.0f;
        estado.erros_validacao++;
        estado.ultimo_erro_validacao = ESP_ERR_INVALID_RESPONSE;
        portEXIT_CRITICAL(&trava_estado);
        xSemaphoreGive(mutex_atualizacao);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const double maior_delta_m =
        delta_esquerda_m > delta_direita_m ? delta_esquerda_m : delta_direita_m;
    const double menor_delta_m =
        delta_esquerda_m < delta_direita_m ? delta_esquerda_m : delta_direita_m;
    const bool amostra_divergente =
        maior_delta_m >= parametros.movimento_minimo_diagnostico_m &&
        menor_delta_m / maior_delta_m < parametros.razao_minima_movimento_rodas;

    bool deve_persistir = false;
    double odometro_atual_m = 0.0;
    portENTER_CRITICAL(&trava_estado);
    estado.roda_esquerda.contagens_acumuladas += novas_contagens_esquerda;
    estado.roda_direita.contagens_acumuladas += novas_contagens_direita;
    estado.roda_esquerda.percurso_m += delta_esquerda_m;
    estado.roda_direita.percurso_m += delta_direita_m;
    estado.roda_esquerda.velocidade_kmh =
        static_cast<float>(velocidade_esquerda_kmh);
    estado.roda_direita.velocidade_kmh =
        static_cast<float>(velocidade_direita_kmh);
    estado.roda_esquerda.ultimo_erro = ESP_OK;
    estado.roda_direita.ultimo_erro = ESP_OK;
    estado.percurso_m += delta_medio_m;
    estado.odometro_m += delta_medio_m;
    estado.velocidade_kmh =
        static_cast<float>(delta_medio_m / delta_s * 3.6);
    estado.atualizacoes++;
    estado.ultimo_erro_leitura = ESP_OK;
    estado.ultimo_erro_validacao = ESP_OK;
    if (amostra_divergente) {
        if (estado.amostras_divergentes_consecutivas < UINT16_MAX) {
            estado.amostras_divergentes_consecutivas++;
        }
        if (estado.amostras_divergentes_consecutivas >=
            parametros.amostras_divergentes_para_alerta) {
            if (estado.rodas_coerentes) estado.divergencias_detectadas++;
            estado.rodas_coerentes = false;
        }
    } else {
        estado.amostras_divergentes_consecutivas = 0;
        estado.rodas_coerentes = true;
    }
    odometro_atual_m = estado.odometro_m;
    estado.persistencia_pendente =
        estado.persistencia_disponivel &&
        odometro_atual_m - odometro_persistido_m >=
            parametros.intervalo_persistencia_m;
    deve_persistir = estado.persistencia_pendente &&
                     agora_us >= proxima_tentativa_persistencia_us &&
                     odometro_atual_m - odometro_persistido_m >=
                         parametros.intervalo_persistencia_m;
    portEXIT_CRITICAL(&trava_estado);

    if (!deve_persistir) {
        xSemaphoreGive(mutex_atualizacao);
        return ESP_OK;
    }

    const esp_err_t erro_persistencia = persistir_odometro(odometro_atual_m);
    portENTER_CRITICAL(&trava_estado);
    if (erro_persistencia == ESP_OK) {
        odometro_persistido_m = odometro_atual_m;
        proxima_tentativa_persistencia_us = 0;
        estado.persistencia_pendente = false;
        estado.ultimo_erro_persistencia = ESP_OK;
    } else {
        estado.erros_persistencia++;
        estado.ultimo_erro_persistencia = erro_persistencia;
        proxima_tentativa_persistencia_us =
            agora_us + INTERVALO_REPETICAO_PERSISTENCIA_US;
    }
    portEXIT_CRITICAL(&trava_estado);
    if (erro_persistencia != ESP_OK) {
        ESP_LOGW(ETIQUETA, "Falha ao persistir odômetro: %s",
                 esp_err_to_name(erro_persistencia));
    }
    xSemaphoreGive(mutex_atualizacao);
    return erro_persistencia;
}

EstadoOdometro servico_odometro_obter_estado() {
    portENTER_CRITICAL(&trava_estado);
    const EstadoOdometro copia = estado;
    portEXIT_CRITICAL(&trava_estado);
    return copia;
}

esp_err_t servico_odometro_zerar_percurso() {
    if (mutex_atualizacao == nullptr ||
        xSemaphoreTake(mutex_atualizacao, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&trava_estado);
    if (!estado.iniciado) {
        portEXIT_CRITICAL(&trava_estado);
        xSemaphoreGive(mutex_atualizacao);
        return ESP_ERR_INVALID_STATE;
    }
    estado.roda_esquerda.percurso_m = 0.0;
    estado.roda_direita.percurso_m = 0.0;
    estado.percurso_m = 0.0;
    portEXIT_CRITICAL(&trava_estado);
    xSemaphoreGive(mutex_atualizacao);
    return ESP_OK;
}
