// Implementa leitura e estado dos encoders incrementais das rodas.
#include "dispositivos/encoder_incremental.h"

#include <cstddef>
#include <cstdint>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "gerenciadores/gerenciador_contador_pulsos.h"

namespace {
constexpr char ETIQUETA[] = "encoder_incremental";
constexpr uint32_t CONTAGENS_POR_PULSO = 4;
constexpr size_t QUANTIDADE_ENCODERS =
    static_cast<size_t>(EncoderIncrementalId::Quantidade);

EstadoEncoderIncremental estados[QUANTIDADE_ENCODERS]{};
portMUX_TYPE trava_estados = portMUX_INITIALIZER_UNLOCKED;

size_t indice_encoder(EncoderIncrementalId id) {
    return static_cast<size_t>(id);
}

bool id_valido(EncoderIncrementalId id) {
    return indice_encoder(id) < QUANTIDADE_ENCODERS;
}

ContadorPulsosId contador_do_encoder(EncoderIncrementalId id) {
    return id == EncoderIncrementalId::RodaEsquerda
               ? ContadorPulsosId::Primeiro
               : ContadorPulsosId::Segundo;
}

bool pinos_em_conflito(EncoderIncrementalId id,
                       const ConfiguracaoEncoderIncremental& configuracao) {
    for (size_t indice = 0; indice < QUANTIDADE_ENCODERS; ++indice) {
        if (indice == indice_encoder(id) || !estados[indice].inicializado) continue;
        const auto& outro = estados[indice];
        if (configuracao.pino_a == outro.pino_a ||
            configuracao.pino_a == outro.pino_b ||
            configuracao.pino_b == outro.pino_a ||
            configuracao.pino_b == outro.pino_b) {
            return true;
        }
    }
    return false;
}

void atualizar_estatisticas(EncoderIncrementalId id) {
    const EstatisticasContadorPulsos contador =
        gerenciador_contador_pulsos_obter_estatisticas(
            contador_do_encoder(id));
    portENTER_CRITICAL(&trava_estados);
    auto& estado = estados[indice_encoder(id)];
    estado.contagem_atual = contador.contagem_atual;
    estado.transbordamentos_compensados =
        contador.transbordamentos_compensados;
    estado.leituras_ok = contador.leituras_ok;
    estado.erros_leitura = contador.erros_leitura;
    estado.ultimo_erro = contador.ultimo_erro;
    portEXIT_CRITICAL(&trava_estados);
}
}  // namespace

esp_err_t encoder_incremental_iniciar(
    EncoderIncrementalId id, const ConfiguracaoEncoderIncremental& configuracao) {
    if (!id_valido(id) || !GPIO_IS_VALID_GPIO(configuracao.pino_a) ||
        !GPIO_IS_VALID_GPIO(configuracao.pino_b) ||
        configuracao.pino_a == configuracao.pino_b ||
        configuracao.pulsos_por_giro == 0 ||
        configuracao.pulsos_por_giro > UINT32_MAX / CONTAGENS_POR_PULSO) {
        return ESP_ERR_INVALID_ARG;
    }
    if (estados[indice_encoder(id)].inicializado) return ESP_OK;
    if (pinos_em_conflito(id, configuracao)) {
        ESP_LOGE(ETIQUETA, "Pinos repetidos entre os encoders");
        return ESP_ERR_INVALID_ARG;
    }

    ConfiguracaoContadorPulsos contador{};
    contador.pino_a = configuracao.pino_a;
    contador.pino_b = configuracao.pino_b;
    contador.filtro_ruido_ns = configuracao.filtro_ruido_ns;
    const esp_err_t erro = gerenciador_contador_pulsos_iniciar(
        contador_do_encoder(id), contador);
    if (erro != ESP_OK) return erro;

    portENTER_CRITICAL(&trava_estados);
    auto& estado = estados[indice_encoder(id)];
    estado = {};
    estado.inicializado = true;
    estado.pino_a = configuracao.pino_a;
    estado.pino_b = configuracao.pino_b;
    estado.pulsos_por_giro = configuracao.pulsos_por_giro;
    estado.contagens_por_giro =
        configuracao.pulsos_por_giro * CONTAGENS_POR_PULSO;
    portEXIT_CRITICAL(&trava_estados);
    atualizar_estatisticas(id);

    ESP_LOGI(ETIQUETA,
             "%s pronto: A=GPIO%d B=GPIO%d, %lu P/R, %lu contagens/giro",
             encoder_incremental_nome(id), configuracao.pino_a,
             configuracao.pino_b,
             static_cast<unsigned long>(configuracao.pulsos_por_giro),
             static_cast<unsigned long>(configuracao.pulsos_por_giro *
                                        CONTAGENS_POR_PULSO));
    return ESP_OK;
}

esp_err_t encoder_incremental_ler(EncoderIncrementalId id,
                                  uint64_t* contagem) {
    if (!id_valido(id) || contagem == nullptr) return ESP_ERR_INVALID_ARG;
    if (!estados[indice_encoder(id)].inicializado) return ESP_ERR_INVALID_STATE;
    const esp_err_t erro = gerenciador_contador_pulsos_ler(
        contador_do_encoder(id), contagem);
    atualizar_estatisticas(id);
    return erro;
}

EstadoEncoderIncremental encoder_incremental_obter_estado(
    EncoderIncrementalId id) {
    if (!id_valido(id)) return {};
    atualizar_estatisticas(id);
    portENTER_CRITICAL(&trava_estados);
    const EstadoEncoderIncremental copia = estados[indice_encoder(id)];
    portEXIT_CRITICAL(&trava_estados);
    return copia;
}

const char* encoder_incremental_nome(EncoderIncrementalId id) {
    switch (id) {
        case EncoderIncrementalId::RodaEsquerda: return "encoder esquerdo";
        case EncoderIncrementalId::RodaDireita: return "encoder direito";
        case EncoderIncrementalId::Quantidade: break;
    }
    return "encoder desconhecido";
}
