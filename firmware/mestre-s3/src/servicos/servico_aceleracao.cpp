// Implementa aquisição filtrada e calibração do movimento longitudinal.
#include "servicos/servico_aceleracao.h"

#include <algorithm>
#include <cmath>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr char ETIQUETA[] = "servico_aceleracao";
constexpr float GRAVIDADE_PADRAO_MS2 = 9.80665f;
constexpr float DOIS_PI = 6.28318530717958647692f;
constexpr float GRAUS_PARA_RADIANOS = 0.01745329251994329577f;

ConfiguracaoServicoAceleracao parametros{};
EstadoServicoAceleracao estado{};
portMUX_TYPE trava_estado = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t tarefa_handle = nullptr;

struct Vetor3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

Vetor3 aceleracao_vetor(const LeituraMpu6050& leitura) {
    return {leitura.aceleracao_x_g, leitura.aceleracao_y_g,
            leitura.aceleracao_z_g};
}

Vetor3 giroscopio_vetor(const LeituraMpu6050& leitura) {
    return {leitura.giroscopio_x_graus_s,
            leitura.giroscopio_y_graus_s,
            leitura.giroscopio_z_graus_s};
}

Vetor3 somar(const Vetor3& a, const Vetor3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vetor3 subtrair(const Vetor3& a, const Vetor3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vetor3 multiplicar(const Vetor3& vetor, float fator) {
    return {vetor.x * fator, vetor.y * fator, vetor.z * fator};
}

Vetor3 produto_vetorial(const Vetor3& a, const Vetor3& b) {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

float modulo(const Vetor3& vetor) {
    return std::sqrt(vetor.x * vetor.x + vetor.y * vetor.y +
                     vetor.z * vetor.z);
}

Vetor3 ajustar_modulo(const Vetor3& vetor, float modulo_desejado) {
    const float modulo_atual = modulo(vetor);
    if (modulo_atual < 0.000001f) return {};
    return multiplicar(vetor, modulo_desejado / modulo_atual);
}

float componente_eixo(const Vetor3& vetor, EixoAceleracaoVeiculo eixo) {
    switch (eixo) {
        case EixoAceleracaoVeiculo::X: return vetor.x;
        case EixoAceleracaoVeiculo::Y: return vetor.y;
        case EixoAceleracaoVeiculo::Z: return vetor.z;
    }
    return 0.0f;
}

SentidoAceleracaoLongitudinal classificar_sentido(
    float aceleracao_g, SentidoAceleracaoLongitudinal atual) {
    if (atual == SentidoAceleracaoLongitudinal::Frente) {
        if (aceleracao_g <= -parametros.limiar_entrada_sentido_g) {
            return SentidoAceleracaoLongitudinal::Tras;
        }
        return aceleracao_g < parametros.limiar_saida_sentido_g
                   ? SentidoAceleracaoLongitudinal::Neutro
                   : SentidoAceleracaoLongitudinal::Frente;
    }
    if (atual == SentidoAceleracaoLongitudinal::Tras) {
        if (aceleracao_g >= parametros.limiar_entrada_sentido_g) {
            return SentidoAceleracaoLongitudinal::Frente;
        }
        return aceleracao_g > -parametros.limiar_saida_sentido_g
                   ? SentidoAceleracaoLongitudinal::Neutro
                   : SentidoAceleracaoLongitudinal::Tras;
    }
    if (aceleracao_g >= parametros.limiar_entrada_sentido_g) {
        return SentidoAceleracaoLongitudinal::Frente;
    }
    if (aceleracao_g <= -parametros.limiar_entrada_sentido_g) {
        return SentidoAceleracaoLongitudinal::Tras;
    }
    return SentidoAceleracaoLongitudinal::Neutro;
}

float componente_eixo(const LeituraMpu6050& leitura,
                      EixoAceleracaoVeiculo eixo) {
    switch (eixo) {
        case EixoAceleracaoVeiculo::X: return leitura.aceleracao_x_g;
        case EixoAceleracaoVeiculo::Y: return leitura.aceleracao_y_g;
        case EixoAceleracaoVeiculo::Z: return leitura.aceleracao_z_g;
    }
    return 0.0f;
}

bool configuracao_valida(const ConfiguracaoServicoAceleracao& configuracao) {
    return static_cast<uint8_t>(configuracao.eixo_longitudinal) <=
               static_cast<uint8_t>(EixoAceleracaoVeiculo::Z) &&
           (configuracao.sinal_eixo_longitudinal == 1.0f ||
            configuracao.sinal_eixo_longitudinal == -1.0f) &&
           configuracao.frequencia_amostragem_hz >= 10 &&
           configuracao.frequencia_amostragem_hz <= 200 &&
           std::isfinite(configuracao.frequencia_corte_filtro_hz) &&
           configuracao.frequencia_corte_filtro_hz > 0.0f &&
           configuracao.frequencia_corte_filtro_hz <
               configuracao.frequencia_amostragem_hz / 2.0f &&
           configuracao.amostras_calibracao > 0 &&
           std::isfinite(configuracao.limite_giroscopio_calibracao_graus_s) &&
           configuracao.limite_giroscopio_calibracao_graus_s > 0.0f &&
           configuracao.gravidade_minima_calibracao_g > 0.0f &&
           configuracao.gravidade_maxima_calibracao_g >
               configuracao.gravidade_minima_calibracao_g &&
           std::isfinite(configuracao.gravidade_maxima_eixo_longitudinal_g) &&
           configuracao.gravidade_maxima_eixo_longitudinal_g > 0.0f &&
           configuracao.gravidade_maxima_eixo_longitudinal_g < 1.0f &&
           std::isfinite(configuracao.frequencia_correcao_gravidade_hz) &&
           configuracao.frequencia_correcao_gravidade_hz > 0.0f &&
           configuracao.frequencia_correcao_gravidade_hz <
               configuracao.frequencia_amostragem_hz / 2.0f &&
           std::isfinite(
               configuracao.limite_aceleracao_correcao_gravidade_g) &&
           configuracao.limite_aceleracao_correcao_gravidade_g > 0.0f &&
           configuracao.limite_aceleracao_correcao_gravidade_g < 0.5f &&
           std::isfinite(configuracao.limiar_entrada_sentido_g) &&
           std::isfinite(configuracao.limiar_saida_sentido_g) &&
           configuracao.limiar_entrada_sentido_g > 0.0f &&
           configuracao.limiar_entrada_sentido_g < 1.0f &&
           configuracao.limiar_saida_sentido_g > 0.0f &&
           configuracao.limiar_saida_sentido_g <
               configuracao.limiar_entrada_sentido_g &&
           configuracao.tempo_confirmacao_sentido_ms > 0 &&
           configuracao.tempo_confirmacao_sentido_ms <= 1000;
}

bool veiculo_aparentemente_parado(const LeituraMpu6050& leitura) {
    const float modulo_aceleracao = std::sqrt(
        leitura.aceleracao_x_g * leitura.aceleracao_x_g +
        leitura.aceleracao_y_g * leitura.aceleracao_y_g +
        leitura.aceleracao_z_g * leitura.aceleracao_z_g);
    const float maior_giro = std::max(
        std::fabs(leitura.giroscopio_x_graus_s),
        std::max(std::fabs(leitura.giroscopio_y_graus_s),
                 std::fabs(leitura.giroscopio_z_graus_s)));
    return modulo_aceleracao >= parametros.gravidade_minima_calibracao_g &&
           modulo_aceleracao <= parametros.gravidade_maxima_calibracao_g &&
           maior_giro <= parametros.limite_giroscopio_calibracao_graus_s;
}

void tarefa_aquisicao(void*) {
    const TickType_t periodo = std::max<TickType_t>(
        1, pdMS_TO_TICKS(1000u / parametros.frequencia_amostragem_hz));
    TickType_t proximo_ciclo = xTaskGetTickCount();
    Vetor3 soma_aceleracao_calibracao{};
    Vetor3 soma_giroscopio_calibracao{};
    Vetor3 gravidade_estimada{};
    Vetor3 desvio_giroscopio{};
    float modulo_gravidade_calibrada = 1.0f;
    uint16_t amostras_calibracao = 0;
    float aceleracao_filtrada_g = 0.0f;
    int64_t instante_anterior_us = esp_timer_get_time();
    int64_t ultimo_aviso_eixo_us = 0;
    int64_t inicio_candidato_sentido_us = instante_anterior_us;
    bool calibrado_local = false;
    bool eixo_compativel_local = false;
    SentidoAceleracaoLongitudinal sentido_confirmado =
        SentidoAceleracaoLongitudinal::Neutro;
    SentidoAceleracaoLongitudinal candidato_sentido = sentido_confirmado;

    while (true) {
        LeituraMpu6050 leitura{};
        const esp_err_t erro = mpu6050_ler(&leitura);
        const int64_t agora_us = esp_timer_get_time();
        if (erro != ESP_OK) {
            portENTER_CRITICAL(&trava_estado);
            estado.erros_leitura++;
            estado.ultimo_erro = erro;
            portEXIT_CRITICAL(&trava_estado);
            // Não integre no próximo ciclo todo o tempo em que não houve uma
            // leitura válida do giroscópio. Isso evita um salto artificial na
            // orientação estimada após uma falha temporária no barramento I2C.
            instante_anterior_us = agora_us;
            vTaskDelayUntil(&proximo_ciclo, periodo);
            continue;
        }

        const Vetor3 aceleracao_medida = aceleracao_vetor(leitura);
        const Vetor3 giroscopio_medido = giroscopio_vetor(leitura);
        const float longitudinal_medido_g =
            componente_eixo(leitura, parametros.eixo_longitudinal) *
            parametros.sinal_eixo_longitudinal;
        bool calibracao_concluida_agora = false;

        if (!calibrado_local) {
            // Um eixo longitudinal quase alinhado com a gravidade indica que a
            // seleção X/Y/Z não corresponde à montagem física. Sem esta
            // validação, inclinação e vibração vertical seriam anunciadas como
            // aceleração para frente ou para trás.
            eixo_compativel_local =
                std::fabs(longitudinal_medido_g) <=
                parametros.gravidade_maxima_eixo_longitudinal_g;
            const bool aparentemente_parado =
                veiculo_aparentemente_parado(leitura);
            if (aparentemente_parado && eixo_compativel_local) {
                soma_aceleracao_calibracao = somar(
                    soma_aceleracao_calibracao, aceleracao_medida);
                soma_giroscopio_calibracao = somar(
                    soma_giroscopio_calibracao, giroscopio_medido);
                amostras_calibracao++;
            } else {
                soma_aceleracao_calibracao = {};
                soma_giroscopio_calibracao = {};
                amostras_calibracao = 0;
            }
            if (aparentemente_parado && !eixo_compativel_local &&
                (ultimo_aviso_eixo_us == 0 ||
                 agora_us - ultimo_aviso_eixo_us >= 5000000)) {
                ESP_LOGW(ETIQUETA,
                         "Eixo longitudinal selecionado mede %+.3f g em repouso; ajuste X/Y/Z conforme a montagem",
                         longitudinal_medido_g);
                ultimo_aviso_eixo_us = agora_us;
            }
            if (amostras_calibracao >= parametros.amostras_calibracao) {
                const float inverso_amostras =
                    1.0f / static_cast<float>(amostras_calibracao);
                gravidade_estimada = multiplicar(
                    soma_aceleracao_calibracao, inverso_amostras);
                desvio_giroscopio = multiplicar(
                    soma_giroscopio_calibracao, inverso_amostras);
                modulo_gravidade_calibrada = modulo(gravidade_estimada);
                if (modulo_gravidade_calibrada < 0.5f) {
                    soma_aceleracao_calibracao = {};
                    soma_giroscopio_calibracao = {};
                    amostras_calibracao = 0;
                } else {
                    aceleracao_filtrada_g = 0.0f;
                    sentido_confirmado =
                        SentidoAceleracaoLongitudinal::Neutro;
                    candidato_sentido = sentido_confirmado;
                    inicio_candidato_sentido_us = agora_us;
                    // A fase de calibração não possui integração de atitude.
                    // Comece o relógio do estimador nesta própria amostra para
                    // não aplicar retroativamente o tempo gasto calibrando.
                    instante_anterior_us = agora_us;
                    calibrado_local = true;
                    calibracao_concluida_agora = true;
                }
            }
        }

        Vetor3 aceleracao_linear{};
        if (calibrado_local) {
            const float delta_s = std::clamp(static_cast<float>(
                                                  agora_us - instante_anterior_us) /
                                                  1000000.0f,
                                              0.0001f, 0.5f);
            const Vetor3 giroscopio_corrigido_graus_s =
                subtrair(giroscopio_medido, desvio_giroscopio);
            const Vetor3 velocidade_angular_rad_s = multiplicar(
                giroscopio_corrigido_graus_s, GRAUS_PARA_RADIANOS);

            // Um vetor fixo no mundo, expresso no referencial do sensor,
            // evolui por dg/dt = -omega x g. Isso acompanha a projeção da
            // gravidade durante arfagem/rolagem sem confundir inclinação com
            // aceleração longitudinal.
            const Vetor3 derivada_gravidade = multiplicar(
                produto_vetorial(velocidade_angular_rad_s,
                                 gravidade_estimada),
                -1.0f);
            gravidade_estimada = ajustar_modulo(
                somar(gravidade_estimada,
                      multiplicar(derivada_gravidade, delta_s)),
                modulo_gravidade_calibrada);

            aceleracao_linear =
                subtrair(aceleracao_medida, gravidade_estimada);
            const float maior_giro_corrigido = std::max(
                std::fabs(giroscopio_corrigido_graus_s.x),
                std::max(std::fabs(giroscopio_corrigido_graus_s.y),
                         std::fabs(giroscopio_corrigido_graus_s.z)));

            // O acelerômetro só corrige lentamente a deriva do giroscópio em
            // condição quase estática. Durante aceleração real, usar seus
            // dados como referência de inclinação absorveria justamente o
            // sinal longitudinal que queremos medir.
            if (modulo(aceleracao_linear) <=
                    parametros.limite_aceleracao_correcao_gravidade_g &&
                maior_giro_corrigido <=
                    parametros.limite_giroscopio_calibracao_graus_s) {
                const float alfa_gravidade = 1.0f - std::exp(
                    -DOIS_PI * parametros.frequencia_correcao_gravidade_hz *
                    delta_s);
                const Vetor3 referencia_acelerometro = ajustar_modulo(
                    aceleracao_medida, modulo_gravidade_calibrada);
                gravidade_estimada = ajustar_modulo(
                    somar(gravidade_estimada,
                          multiplicar(subtrair(referencia_acelerometro,
                                               gravidade_estimada),
                                      alfa_gravidade)),
                    modulo_gravidade_calibrada);
                aceleracao_linear =
                    subtrair(aceleracao_medida, gravidade_estimada);
            }

            const float aceleracao_longitudinal_sem_gravidade_g =
                componente_eixo(aceleracao_linear,
                                parametros.eixo_longitudinal) *
                parametros.sinal_eixo_longitudinal;
            const float alfa = 1.0f - std::exp(
                -DOIS_PI * parametros.frequencia_corte_filtro_hz * delta_s);
            aceleracao_filtrada_g +=
                alfa * (aceleracao_longitudinal_sem_gravidade_g -
                        aceleracao_filtrada_g);

            const SentidoAceleracaoLongitudinal desejado =
                classificar_sentido(aceleracao_filtrada_g,
                                    sentido_confirmado);
            if (desejado == sentido_confirmado) {
                candidato_sentido = sentido_confirmado;
                inicio_candidato_sentido_us = agora_us;
            } else if (desejado != candidato_sentido) {
                candidato_sentido = desejado;
                inicio_candidato_sentido_us = agora_us;
            } else if (agora_us - inicio_candidato_sentido_us >=
                       static_cast<int64_t>(
                           parametros.tempo_confirmacao_sentido_ms) *
                           1000) {
                sentido_confirmado = candidato_sentido;
            }
        }
        instante_anterior_us = agora_us;

        portENTER_CRITICAL(&trava_estado);
        estado.leitura = leitura;
        estado.leituras_ok++;
        estado.ultimo_erro = ESP_OK;
        estado.calibrado = calibrado_local;
        estado.eixo_longitudinal_compativel = eixo_compativel_local;
        estado.amostras_calibracao_coletadas = amostras_calibracao;
        estado.componente_gravidade_longitudinal_g = componente_eixo(
            gravidade_estimada, parametros.eixo_longitudinal) *
            parametros.sinal_eixo_longitudinal;
        estado.gravidade_estimada_x_g = gravidade_estimada.x;
        estado.gravidade_estimada_y_g = gravidade_estimada.y;
        estado.gravidade_estimada_z_g = gravidade_estimada.z;
        estado.aceleracao_linear_x_g = aceleracao_linear.x;
        estado.aceleracao_linear_y_g = aceleracao_linear.y;
        estado.aceleracao_linear_z_g = aceleracao_linear.z;
        estado.desvio_giroscopio_x_graus_s = desvio_giroscopio.x;
        estado.desvio_giroscopio_y_graus_s = desvio_giroscopio.y;
        estado.desvio_giroscopio_z_graus_s = desvio_giroscopio.z;
        estado.aceleracao_longitudinal_g =
            calibrado_local ? aceleracao_filtrada_g : 0.0f;
        estado.aceleracao_longitudinal_ms2 =
            estado.aceleracao_longitudinal_g * GRAVIDADE_PADRAO_MS2;
        estado.sentido = calibrado_local
                             ? sentido_confirmado
                             : SentidoAceleracaoLongitudinal::Neutro;
        portEXIT_CRITICAL(&trava_estado);

        if (calibracao_concluida_agora) {
            ESP_LOGI(ETIQUETA,
                     "Calibracao concluida: gravidade=(%+.4f,%+.4f,%+.4f) g, desvio giro=(%+.3f,%+.3f,%+.3f) graus/s",
                     gravidade_estimada.x, gravidade_estimada.y,
                     gravidade_estimada.z, desvio_giroscopio.x,
                     desvio_giroscopio.y, desvio_giroscopio.z);
        }
        vTaskDelayUntil(&proximo_ciclo, periodo);
    }
}
}  // namespace

esp_err_t servico_aceleracao_iniciar(
    const ConfiguracaoServicoAceleracao& configuracao) {
    if (tarefa_handle != nullptr) return ESP_OK;
    if (!configuracao_valida(configuracao)) return ESP_ERR_INVALID_ARG;

    const esp_err_t erro = mpu6050_iniciar(
        configuracao.frequencia_amostragem_hz);
    if (erro != ESP_OK) {
        portENTER_CRITICAL(&trava_estado);
        estado = {};
        estado.ultimo_erro = erro;
        portEXIT_CRITICAL(&trava_estado);
        return erro;
    }

    // Confirma o caminho completo de dados antes de anunciar o serviço como
    // disponível. Assim WHO_AM_I correto, mas registradores de dados inválidos,
    // não passam silenciosamente pela inicialização.
    LeituraMpu6050 primeira_leitura{};
    const esp_err_t erro_primeira_leitura = mpu6050_ler(&primeira_leitura);
    if (erro_primeira_leitura != ESP_OK) {
        portENTER_CRITICAL(&trava_estado);
        estado = {};
        estado.ultimo_erro = erro_primeira_leitura;
        portEXIT_CRITICAL(&trava_estado);
        return erro_primeira_leitura;
    }

    parametros = configuracao;
    portENTER_CRITICAL(&trava_estado);
    estado = {};
    estado.iniciado = true;
    estado.leitura = primeira_leitura;
    estado.leituras_ok = 1;
    portEXIT_CRITICAL(&trava_estado);

    if (xTaskCreate(tarefa_aquisicao, "aceleracao", 4096, nullptr, 5,
                    &tarefa_handle) != pdPASS) {
        tarefa_handle = nullptr;
        portENTER_CRITICAL(&trava_estado);
        estado.iniciado = false;
        portEXIT_CRITICAL(&trava_estado);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(ETIQUETA,
             "Amostragem=%lu Hz, filtro=%.1f Hz; mantenha o veiculo parado durante %u amostras",
             static_cast<unsigned long>(parametros.frequencia_amostragem_hz),
             parametros.frequencia_corte_filtro_hz,
             static_cast<unsigned>(parametros.amostras_calibracao));
    return ESP_OK;
}

EstadoServicoAceleracao servico_aceleracao_obter_estado() {
    portENTER_CRITICAL(&trava_estado);
    const EstadoServicoAceleracao copia = estado;
    portEXIT_CRITICAL(&trava_estado);
    return copia;
}
