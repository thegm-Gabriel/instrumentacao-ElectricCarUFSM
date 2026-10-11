// Implementa contagem PCNT de 64 bits e compensação de limites.
#include "gerenciadores/gerenciador_contador_pulsos.h"

#include <cstddef>

#include "driver/pulse_cnt.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

namespace {
constexpr char ETIQUETA[] = "contador_pulsos";
constexpr int LIMITE_CONTADOR = 30000;
constexpr size_t QUANTIDADE_CONTADORES =
    static_cast<size_t>(ContadorPulsosId::Quantidade);

struct InstanciaContador {
    pcnt_unit_handle_t unidade = nullptr;
    pcnt_channel_handle_t canal_a = nullptr;
    pcnt_channel_handle_t canal_b = nullptr;
    uint64_t contagem_base = 0;
    uint32_t geracao_contagem = 0;
    EstatisticasContadorPulsos estatisticas{};
};

InstanciaContador instancias[QUANTIDADE_CONTADORES]{};
portMUX_TYPE trava_estatisticas = portMUX_INITIALIZER_UNLOCKED;

bool IRAM_ATTR tratar_limite_contador(
    pcnt_unit_handle_t, const pcnt_watch_event_data_t* evento,
    void* contexto) {
    auto* instancia = static_cast<InstanciaContador*>(contexto);
    if (instancia == nullptr || evento == nullptr ||
        evento->watch_point_value != LIMITE_CONTADOR) {
        return false;
    }
    portENTER_CRITICAL_ISR(&trava_estatisticas);
    instancia->contagem_base += LIMITE_CONTADOR;
    instancia->geracao_contagem++;
    instancia->estatisticas.transbordamentos_compensados++;
    portEXIT_CRITICAL_ISR(&trava_estatisticas);
    return false;
}

InstanciaContador* obter_instancia(ContadorPulsosId id) {
    const size_t indice = static_cast<size_t>(id);
    return indice < QUANTIDADE_CONTADORES ? &instancias[indice] : nullptr;
}

void registrar_erro(InstanciaContador& instancia, esp_err_t erro) {
    portENTER_CRITICAL(&trava_estatisticas);
    instancia.estatisticas.erros_leitura++;
    instancia.estatisticas.ultimo_erro = erro;
    portEXIT_CRITICAL(&trava_estatisticas);
}

void liberar_recursos(InstanciaContador& instancia, bool unidade_habilitada) {
    if (unidade_habilitada && instancia.unidade != nullptr) {
        pcnt_unit_disable(instancia.unidade);
    }
    if (instancia.canal_b != nullptr) {
        pcnt_del_channel(instancia.canal_b);
        instancia.canal_b = nullptr;
    }
    if (instancia.canal_a != nullptr) {
        pcnt_del_channel(instancia.canal_a);
        instancia.canal_a = nullptr;
    }
    if (instancia.unidade != nullptr) {
        pcnt_del_unit(instancia.unidade);
        instancia.unidade = nullptr;
    }
}

esp_err_t configurar_canal(InstanciaContador& instancia, gpio_num_t pino,
                           pcnt_channel_handle_t* canal) {
    pcnt_chan_config_t parametros{};
    parametros.edge_gpio_num = pino;
    parametros.level_gpio_num = -1;
    esp_err_t erro = pcnt_new_channel(instancia.unidade, &parametros, canal);
    if (erro == ESP_OK) {
        erro = pcnt_channel_set_edge_action(
            *canal, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
            PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    }
    if (erro == ESP_OK) {
        erro = pcnt_channel_set_level_action(
            *canal, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
            PCNT_CHANNEL_LEVEL_ACTION_KEEP);
    }
    return erro;
}
}  // namespace

esp_err_t gerenciador_contador_pulsos_iniciar(
    ContadorPulsosId id, const ConfiguracaoContadorPulsos& configuracao) {
    InstanciaContador* instancia = obter_instancia(id);
    if (instancia == nullptr) return ESP_ERR_INVALID_ARG;
    if (instancia->estatisticas.inicializado) return ESP_OK;
    if (!GPIO_IS_VALID_GPIO(configuracao.pino_a) ||
        !GPIO_IS_VALID_GPIO(configuracao.pino_b) ||
        configuracao.pino_a == configuracao.pino_b) {
        return ESP_ERR_INVALID_ARG;
    }

    pcnt_unit_config_t parametros_unidade{};
    parametros_unidade.low_limit = -LIMITE_CONTADOR;
    parametros_unidade.high_limit = LIMITE_CONTADOR;
    // O acumulador do driver retorna int. A extensão para 64 bits é mantida
    // por este gerenciador a cada evento de limite do PCNT.
    parametros_unidade.flags.accum_count = false;

    esp_err_t erro = pcnt_new_unit(&parametros_unidade, &instancia->unidade);
    if (erro == ESP_OK && configuracao.filtro_ruido_ns > 0) {
        pcnt_glitch_filter_config_t filtro{};
        filtro.max_glitch_ns = configuracao.filtro_ruido_ns;
        erro = pcnt_unit_set_glitch_filter(instancia->unidade, &filtro);
    }
    if (erro == ESP_OK) {
        erro = configurar_canal(*instancia, configuracao.pino_a,
                                &instancia->canal_a);
    }
    if (erro == ESP_OK) {
        erro = configurar_canal(*instancia, configuracao.pino_b,
                                &instancia->canal_b);
    }

    if (erro == ESP_OK) {
        erro = pcnt_unit_add_watch_point(instancia->unidade, LIMITE_CONTADOR);
    }
    if (erro == ESP_OK) {
        pcnt_event_callbacks_t callbacks{};
        callbacks.on_reach = tratar_limite_contador;
        erro = pcnt_unit_register_event_callbacks(
            instancia->unidade, &callbacks, instancia);
    }
    if (erro == ESP_OK) erro = pcnt_unit_enable(instancia->unidade);
    const bool unidade_habilitada = erro == ESP_OK;
    if (erro == ESP_OK) erro = pcnt_unit_clear_count(instancia->unidade);
    if (erro == ESP_OK) {
        portENTER_CRITICAL(&trava_estatisticas);
        instancia->contagem_base = 0;
        instancia->geracao_contagem = 0;
        instancia->estatisticas = {};
        portEXIT_CRITICAL(&trava_estatisticas);
    }
    if (erro == ESP_OK) erro = pcnt_unit_start(instancia->unidade);

    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Falha ao iniciar PCNT %u: %s",
                 static_cast<unsigned>(id), esp_err_to_name(erro));
        liberar_recursos(*instancia, unidade_habilitada);
        registrar_erro(*instancia, erro);
        return erro;
    }

    portENTER_CRITICAL(&trava_estatisticas);
    instancia->estatisticas.inicializado = true;
    portEXIT_CRITICAL(&trava_estatisticas);
    ESP_LOGI(ETIQUETA,
             "PCNT %u ativo: A=GPIO%d B=GPIO%d, quatro contagens por pulso, acumulador de 64 bits",
             static_cast<unsigned>(id), configuracao.pino_a,
             configuracao.pino_b);
    return ESP_OK;
}

esp_err_t gerenciador_contador_pulsos_ler(ContadorPulsosId id,
                                          uint64_t* contagem) {
    if (contagem == nullptr) return ESP_ERR_INVALID_ARG;
    InstanciaContador* instancia = obter_instancia(id);
    if (instancia == nullptr) return ESP_ERR_INVALID_ARG;
    if (!instancia->estatisticas.inicializado || instancia->unidade == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    int valor_hardware = 0;
    uint64_t base_depois = 0;
    uint32_t geracao_antes = 0;
    uint32_t geracao_depois = 0;
    esp_err_t erro = ESP_OK;
    do {
        portENTER_CRITICAL(&trava_estatisticas);
        geracao_antes = instancia->geracao_contagem;
        portEXIT_CRITICAL(&trava_estatisticas);

        erro = pcnt_unit_get_count(instancia->unidade, &valor_hardware);
        if (erro != ESP_OK) {
            registrar_erro(*instancia, erro);
            return erro;
        }

        portENTER_CRITICAL(&trava_estatisticas);
        base_depois = instancia->contagem_base;
        geracao_depois = instancia->geracao_contagem;
        portEXIT_CRITICAL(&trava_estatisticas);
    } while (geracao_antes != geracao_depois);

    if (valor_hardware < 0) {
        registrar_erro(*instancia, ESP_ERR_INVALID_RESPONSE);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint64_t valor_total = base_depois +
                                 static_cast<uint64_t>(valor_hardware);
    *contagem = valor_total;
    portENTER_CRITICAL(&trava_estatisticas);
    instancia->estatisticas.contagem_atual = valor_total;
    instancia->estatisticas.leituras_ok++;
    instancia->estatisticas.ultimo_erro = ESP_OK;
    portEXIT_CRITICAL(&trava_estatisticas);
    return ESP_OK;
}

EstatisticasContadorPulsos gerenciador_contador_pulsos_obter_estatisticas(
    ContadorPulsosId id) {
    InstanciaContador* instancia = obter_instancia(id);
    if (instancia == nullptr) return {};
    portENTER_CRITICAL(&trava_estatisticas);
    const EstatisticasContadorPulsos copia = instancia->estatisticas;
    portEXIT_CRITICAL(&trava_estatisticas);
    return copia;
}
