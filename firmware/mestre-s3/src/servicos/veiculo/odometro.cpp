// Implementa distância, velocidade, coerência das rodas e persistência.
#include "servicos/veiculo/odometro.h"
#include "servicos/manutencao/ajustes.h"
#include "nucleo/configuracao/tarefas.h"
#include "nucleo/configuracao/veiculo.h"

#include <algorithm>
#include <cmath>

#include "dispositivos/entradas/encoder.h"
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
constexpr int64_t PRAZO_VELOCIDADE_US = configuracao::PRAZO_VELOCIDADE_MS * 1000LL;
static_assert(configuracao::PERIODO_ODOMETRO_MS < configuracao::PRAZO_VELOCIDADE_MS);

EstadoOdometro estado{};
ConfiguracaoOdometro parametros{};
portMUX_TYPE trava_estado = portMUX_INITIALIZER_UNLOCKED;
uint64_t contagem_anterior_esquerda = 0;
uint64_t contagem_anterior_direita = 0;
int64_t instante_anterior_us = 0;
double odometro_persistido_m = 0.0;
nvs_handle_t armazenamento = 0;
SemaphoreHandle_t mutex_persistencia = nullptr;
int64_t proxima_tentativa_persistencia_us = 0;
uint32_t geracao_atividade = 0;
uint32_t geracao_amostrada = 0;
double distancia_diagnostico_esquerda = 0.0;
double distancia_diagnostico_direita = 0.0;
int64_t janela_diagnostico_us = 0;

bool configuracao_valida(const ConfiguracaoOdometro& configuracao) {
    return std::isfinite(configuracao.intervalo_persistencia_m) &&
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

struct EscalasRodas {
    double esquerda;
    double direita;
};

double calcular_metros_por_contagem(float diametro_m, float giros_encoder,
                                   uint32_t pulsos_por_giro, float fator) {
    const double contagens_por_giro_roda =
        static_cast<double>(pulsos_por_giro) * CONTAGENS_POR_PULSO_ENCODER * giros_encoder;
    return (PI * diametro_m * fator) / contagens_por_giro_roda;
}

EscalasRodas calcular_escalas(const ajustes_desenvolvedor_t& ajustes) {
    return {
        calcular_metros_por_contagem(
            ajustes.diametro_roda_esquerda_m, ajustes.giros_encoder_esquerdo_por_giro_roda,
            ajustes.pulsos_encoder_esquerdo_por_giro, ajustes.fator_encoder_esquerdo),
        calcular_metros_por_contagem(
            ajustes.diametro_roda_direita_m, ajustes.giros_encoder_direito_por_giro_roda,
            ajustes.pulsos_encoder_direito_por_giro, ajustes.fator_encoder_direito),
    };
}

void informar_resolucao_encoders(const ajustes_desenvolvedor_t& ajustes) {
    encoder_incremental_definir_resolucao(EncoderIncrementalId::RodaEsquerda,
                                         ajustes.pulsos_encoder_esquerdo_por_giro);
    encoder_incremental_definir_resolucao(EncoderIncrementalId::RodaDireita,
                                         ajustes.pulsos_encoder_direito_por_giro);
}

esp_err_t ler_encoder_configurado(EncoderIncrementalId id, bool habilitado,
                                  uint64_t& contagem) {
    if (habilitado) return encoder_incremental_ler(id, &contagem);
    contagem = 0;
    return ESP_OK;
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

void amostrar_rodas();

void tarefa_odometro(void*) {
    const TickType_t periodo = pdMS_TO_TICKS(configuracao::PERIODO_ODOMETRO_MS);
    configASSERT(periodo > 0);
    TickType_t ultimo_ciclo = xTaskGetTickCount();
    while (true) {
        if (xTaskDelayUntil(&ultimo_ciclo, periodo) == pdFALSE) {
            ultimo_ciclo = xTaskGetTickCount();
        }
        amostrar_rodas();
    }
}
}  // namespace

esp_err_t servico_odometro_iniciar(const ConfiguracaoOdometro& configuracao) {
    if (estado.iniciado) return ESP_OK;
    if (!configuracao_valida(configuracao)) return ESP_ERR_INVALID_ARG;
    if (mutex_persistencia == nullptr) {
        mutex_persistencia = xSemaphoreCreateMutex();
    }
    if (mutex_persistencia == nullptr) return ESP_ERR_NO_MEM;

    uint64_t contagem_inicial_esquerda = 0;
    uint64_t contagem_inicial_direita = 0;
    esp_err_t erro = ler_encoder_configurado(EncoderIncrementalId::RodaEsquerda,
        configuracao::USAR_ENCODER_ESQUERDO, contagem_inicial_esquerda);
    if (erro == ESP_OK) {
        erro = ler_encoder_configurado(EncoderIncrementalId::RodaDireita,
            configuracao::USAR_ENCODER_DIREITO, contagem_inicial_direita);
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

    const auto ajustes = ajustes_desenvolvedor_obter();
    const auto escalas = calcular_escalas(ajustes);
    informar_resolucao_encoders(ajustes);
    parametros = configuracao;
    contagem_anterior_esquerda = contagem_inicial_esquerda;
    contagem_anterior_direita = contagem_inicial_direita;
    instante_anterior_us = esp_timer_get_time();
    odometro_persistido_m = odometro_inicial_m;

    portENTER_CRITICAL(&trava_estado);
    const bool ativo = estado.ativo;
    estado = {};
    estado.ativo = ativo;
    estado.iniciado = true;
    estado.rodas_coerentes = true;
    estado.persistencia_disponivel = persistencia_disponivel;
    estado.odometro_m = odometro_inicial_m;
    estado.ultimo_erro_persistencia =
        persistencia_disponivel ? ESP_OK : erro;
    estado.revisao_geometria = ajustes.revisao_geometria;
    estado.roda_esquerda.metros_por_contagem = escalas.esquerda;
    estado.roda_direita.metros_por_contagem = escalas.direita;
    portEXIT_CRITICAL(&trava_estado);

    if (configuracao::criar_tarefa(tarefa_odometro, "odometro", nullptr, nullptr) != pdPASS) {
        portENTER_CRITICAL(&trava_estado);
        estado.iniciado = false;
        estado.persistencia_disponivel = false;
        portEXIT_CRITICAL(&trava_estado);
        if (armazenamento != 0) nvs_close(armazenamento);
        armazenamento = 0;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(ETIQUETA,
             "Odômetro ativo: encoders %s; resolução esquerda=%.6f m, direita=%.6f m; total=média das rodas",
             configuracao::USAR_ENCODER_ESQUERDO && configuracao::USAR_ENCODER_DIREITO
                 ? "esquerdo e direito"
                 : configuracao::USAR_ENCODER_ESQUERDO ? "esquerdo" : "direito",
             estado.roda_esquerda.metros_por_contagem,
             estado.roda_direita.metros_por_contagem);
    return ESP_OK;
}

namespace {
void amostrar_rodas() {
    portENTER_CRITICAL(&trava_estado);
    const bool ativo = estado.ativo;
    const uint32_t geracao = geracao_atividade;
    const uint32_t revisao_geometria = estado.revisao_geometria;
    portEXIT_CRITICAL(&trava_estado);
    if (!ativo) return;

    uint64_t contagem_esquerda = 0;
    uint64_t contagem_direita = 0;
    const esp_err_t erro_esquerda = ler_encoder_configurado(EncoderIncrementalId::RodaEsquerda,
        configuracao::USAR_ENCODER_ESQUERDO, contagem_esquerda);
    const esp_err_t erro_direita = ler_encoder_configurado(EncoderIncrementalId::RodaDireita,
        configuracao::USAR_ENCODER_DIREITO, contagem_direita);
    const int64_t agora_us = esp_timer_get_time();
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
        estado.instante_velocidade_us = 0;
        estado.parado_desde_us = 0;
        portEXIT_CRITICAL(&trava_estado);
        return;
    }

    const auto ajustes = ajustes_desenvolvedor_obter();
    const bool mudou_geometria = revisao_geometria != ajustes.revisao_geometria;
    const auto escalas = mudou_geometria ? calcular_escalas(ajustes) : EscalasRodas{};
    const int64_t delta_us = agora_us - instante_anterior_us;
    // Um ciclo atrasado pode ser seguido por outro quase imediato.
    if (delta_us < configuracao::PERIODO_ODOMETRO_MS * 500LL) return;
    const bool intervalo_recente = delta_us <= PRAZO_VELOCIDADE_US &&
        geracao == geracao_amostrada;
    const uint64_t novas_contagens_esquerda =
        contagem_esquerda >= contagem_anterior_esquerda
            ? contagem_esquerda - contagem_anterior_esquerda : UINT64_MAX;
    const uint64_t novas_contagens_direita =
        contagem_direita >= contagem_anterior_direita
            ? contagem_direita - contagem_anterior_direita : UINT64_MAX;

    portENTER_CRITICAL(&trava_estado);
    if (geracao != geracao_atividade) {
        portEXIT_CRITICAL(&trava_estado);
        return;
    }
    contagem_anterior_esquerda = contagem_esquerda;
    contagem_anterior_direita = contagem_direita;
    instante_anterior_us = agora_us;
    geracao_amostrada = geracao;

    // Recomeça dos contadores atuais. Pulsos e tempo do intervalo em que o
    // ajuste mudou não entram nos totais nem na primeira velocidade nova.
    if (mudou_geometria) {
        estado.revisao_geometria = ajustes.revisao_geometria;
        estado.roda_esquerda.metros_por_contagem = escalas.esquerda;
        estado.roda_direita.metros_por_contagem = escalas.direita;
        estado.roda_esquerda.velocidade_kmh = 0.0f;
        estado.roda_direita.velocidade_kmh = 0.0f;
        estado.velocidade_kmh = 0.0f;
        estado.instante_velocidade_us = 0;
        estado.parado_desde_us = 0;
        estado.amostras_divergentes_consecutivas = 0;
        distancia_diagnostico_esquerda = distancia_diagnostico_direita = 0.0;
        janela_diagnostico_us = 0;
        portEXIT_CRITICAL(&trava_estado);
        informar_resolucao_encoders(ajustes);
        return;
    }

    const double delta_esquerda_m =
        novas_contagens_esquerda * estado.roda_esquerda.metros_por_contagem;
    const double delta_direita_m =
        novas_contagens_direita * estado.roda_direita.metros_por_contagem;
    const double delta_medio_m = (delta_esquerda_m + delta_direita_m) / 2.0;
    const double delta_s = static_cast<double>(delta_us) / 1000000.0;

    const double velocidade_esquerda_kmh =
        delta_esquerda_m / delta_s * 3.6;
    const double velocidade_direita_kmh =
        delta_direita_m / delta_s * 3.6;
    if (novas_contagens_esquerda == UINT64_MAX || novas_contagens_direita == UINT64_MAX ||
        velocidade_esquerda_kmh > parametros.velocidade_maxima_kmh ||
        velocidade_direita_kmh > parametros.velocidade_maxima_kmh) {
        estado.roda_esquerda.velocidade_kmh = 0.0f;
        estado.roda_direita.velocidade_kmh = 0.0f;
        estado.velocidade_kmh = 0.0f;
        estado.instante_velocidade_us = 0;
        estado.parado_desde_us = 0;
        estado.erros_validacao++;
        estado.ultimo_erro_validacao = ESP_ERR_INVALID_RESPONSE;
        portEXIT_CRITICAL(&trava_estado);
        return;
    }

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
    estado.instante_velocidade_us = intervalo_recente ? agora_us : 0;
    if (!intervalo_recente || novas_contagens_esquerda != 0 || novas_contagens_direita != 0)
        estado.parado_desde_us = 0;
    else if (estado.parado_desde_us == 0)
        estado.parado_desde_us = agora_us;
    estado.atualizacoes++;
    estado.ultimo_erro_leitura = ESP_OK;
    estado.ultimo_erro_validacao = ESP_OK;
    // A divergência só é verificável quando os dois encoders estão instalados.
    constexpr bool comparar_rodas = configuracao::USAR_ENCODER_ESQUERDO && configuracao::USAR_ENCODER_DIREITO;
    if (comparar_rodas && !intervalo_recente) {
        distancia_diagnostico_esquerda = distancia_diagnostico_direita = 0.0;
        janela_diagnostico_us = 0;
        estado.amostras_divergentes_consecutivas = 0;
    } else if (comparar_rodas) {
        distancia_diagnostico_esquerda += delta_esquerda_m;
        distancia_diagnostico_direita += delta_direita_m;
        janela_diagnostico_us += delta_us;
        if (janela_diagnostico_us >= configuracao::JANELA_DIAGNOSTICO_RODAS_MS * 1000LL) {
            const double maior = std::max(distancia_diagnostico_esquerda, distancia_diagnostico_direita);
            const double menor = std::min(distancia_diagnostico_esquerda, distancia_diagnostico_direita);
            const bool divergente = maior >= parametros.movimento_minimo_diagnostico_m &&
                menor < maior * parametros.razao_minima_movimento_rodas;
            if (divergente) {
                if (estado.amostras_divergentes_consecutivas < UINT16_MAX)
                    ++estado.amostras_divergentes_consecutivas;
                if (estado.amostras_divergentes_consecutivas >= parametros.amostras_divergentes_para_alerta) {
                    if (estado.rodas_coerentes) ++estado.divergencias_detectadas;
                    estado.rodas_coerentes = false;
                }
            } else {
                estado.amostras_divergentes_consecutivas = 0;
                estado.rodas_coerentes = true;
            }
            distancia_diagnostico_esquerda = distancia_diagnostico_direita = 0.0;
            janela_diagnostico_us = 0;
        }
    }
    estado.persistencia_pendente =
        estado.persistencia_disponivel &&
        estado.odometro_m - odometro_persistido_m >=
            parametros.intervalo_persistencia_m;
    portEXIT_CRITICAL(&trava_estado);
}
}  // namespace

void servico_odometro_definir_ativo(bool ativo) {
    portENTER_CRITICAL(&trava_estado);
    if (estado.ativo != ativo) {
        estado.ativo = ativo;
        estado.instante_velocidade_us = 0;
        estado.parado_desde_us = 0;
        ++geracao_atividade;
    }
    portEXIT_CRITICAL(&trava_estado);
}

bool servico_odometro_amostra_recente(const EstadoOdometro& amostra, int64_t agora_us) {
    return amostra.iniciado && amostra.ativo &&
        amostra.ultimo_erro_leitura == ESP_OK && amostra.ultimo_erro_validacao == ESP_OK &&
        amostra.instante_velocidade_us > 0 && agora_us >= amostra.instante_velocidade_us &&
        agora_us - amostra.instante_velocidade_us <= PRAZO_VELOCIDADE_US &&
        amostra.revisao_geometria == ajustes_desenvolvedor_revisao_geometria();
}

bool servico_odometro_velocidade_valida(const EstadoOdometro& amostra,
                                       int64_t agora_us) {
    return amostra.rodas_coerentes && servico_odometro_amostra_recente(amostra, agora_us);
}

void servico_odometro_persistir_pendente() {
    if (mutex_persistencia == nullptr || xSemaphoreTake(mutex_persistencia, 0) != pdTRUE) return;
    const int64_t agora_us = esp_timer_get_time();
    portENTER_CRITICAL(&trava_estado);
    const bool deve_persistir = estado.persistencia_pendente &&
        agora_us >= proxima_tentativa_persistencia_us;
    const double odometro_atual_m = estado.odometro_m;
    portEXIT_CRITICAL(&trava_estado);
    if (!deve_persistir) {
        xSemaphoreGive(mutex_persistencia);
        return;
    }
    const esp_err_t erro_persistencia = persistir_odometro(odometro_atual_m);
    portENTER_CRITICAL(&trava_estado);
    if (erro_persistencia == ESP_OK) {
        odometro_persistido_m = odometro_atual_m;
        proxima_tentativa_persistencia_us = 0;
        estado.persistencia_pendente = estado.odometro_m - odometro_persistido_m >=
            parametros.intervalo_persistencia_m;
        estado.ultimo_erro_persistencia = ESP_OK;
    } else {
        estado.erros_persistencia++;
        estado.ultimo_erro_persistencia = erro_persistencia;
        proxima_tentativa_persistencia_us =
            agora_us + configuracao::INTERVALO_REPETICAO_PERSISTENCIA_US;
    }
    portEXIT_CRITICAL(&trava_estado);
    if (erro_persistencia != ESP_OK) {
        ESP_LOGW(ETIQUETA, "Falha ao persistir odômetro: %s",
                 esp_err_to_name(erro_persistencia));
    }
    xSemaphoreGive(mutex_persistencia);
}

EstadoOdometro servico_odometro_obter_estado() {
    portENTER_CRITICAL(&trava_estado);
    const EstadoOdometro copia = estado;
    portEXIT_CRITICAL(&trava_estado);
    return copia;
}

esp_err_t servico_odometro_restaurar_se_ausente(uint64_t total_mm) {
    if (mutex_persistencia == nullptr ||
        xSemaphoreTake(mutex_persistencia, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t resultado = ESP_ERR_INVALID_STATE;
    if (estado.iniciado && armazenamento != 0) {
        uint64_t atual_mm = 0;
        resultado = nvs_get_u64(armazenamento, CHAVE_ODOMETRO_MM, &atual_mm);
        if (resultado == ESP_ERR_NVS_NOT_FOUND) {
            const double metros = static_cast<double>(total_mm) / 1000.0;
            resultado = persistir_odometro(metros);
            if (resultado == ESP_OK) {
                portENTER_CRITICAL(&trava_estado);
                estado.odometro_m += metros;
                estado.persistencia_pendente = estado.odometro_m - metros >=
                    parametros.intervalo_persistencia_m;
                estado.ultimo_erro_persistencia = ESP_OK;
                odometro_persistido_m = metros;
                portEXIT_CRITICAL(&trava_estado);
                ESP_LOGW(ETIQUETA, "Odômetro restaurado do backup SD: %.3f km",
                         metros / 1000.0);
            }
        }
    }
    xSemaphoreGive(mutex_persistencia);
    return resultado;
}

esp_err_t servico_odometro_zerar_percurso() {
    portENTER_CRITICAL(&trava_estado);
    if (!estado.iniciado) {
        portEXIT_CRITICAL(&trava_estado);
        return ESP_ERR_INVALID_STATE;
    }
    estado.roda_esquerda.percurso_m = 0.0;
    estado.roda_direita.percurso_m = 0.0;
    estado.percurso_m = 0.0;
    portEXIT_CRITICAL(&trava_estado);
    return ESP_OK;
}
