#include "servicos/veiculo/controle.h"
#include "servicos/manutencao/ajustes.h"

#include <algorithm>
#include <atomic>
#include <cmath>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "dispositivos/entradas/adc.h"
#include "dispositivos/saidas/mcp4725.h"
#include "dispositivos/saidas/gpio_veiculo.h"
#include "dispositivos/entradas/chave_marcha.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nucleo/configuracao/ota.h"
#include "nucleo/configuracao/tarefas.h"
#include "nucleo/configuracao/placa.h"
#include "nucleo/configuracao/veiculo.h"
#include "nucleo/tempo.h"
#include "servicos/veiculo/aquisicao.h"
#include "servicos/veiculo/aceleracao.h"
#include "servicos/veiculo/odometro.h"
#include "servicos/veiculo/simulador.h"

namespace {
constexpr char ETIQUETA[] = "controle_remoto";
constexpr char ESPACO_NVS_CONTROLE[] = "controle";
constexpr char CHAVE_VELOCIDADE[] = "velocidade";
constexpr char CHAVE_ACELERADOR[] = "acelerador";
constexpr char CHAVE_MARCHA[] = "marcha";
constexpr char CHAVE_CALIBRACAO[] = "calibracao";
constexpr char CHAVE_CALIBRACAO_EXTRAS[] = "calib_ext";
constexpr char CHAVE_LED_INTEGRADO[] = "led_integrado";
constexpr uint32_t VERSAO_CALIBRACAO_NVS_ANTERIOR = 1;
constexpr uint32_t VERSAO_CALIBRACAO_NVS = 2;
constexpr uint32_t INTERVALO_DAC_PADRAO_ANTERIOR_MS = 100;

// Os limites operacionais têm chaves próprias. A calibração é gravada em conjunto.
struct CalibracaoNvs {
    uint32_t versao;
    int32_t valores[AJUSTE_limite_acelerador_decimos];
};
static_assert(configuracao::DAC_TENSAO_MINIMA_SAIDA_MV <=
              CONTROLE_REMOTO_TENSAO_MINIMA_MAXIMA_MV);
static_assert(CONTROLE_REMOTO_TENSAO_MINIMA_MAXIMA_MV < configuracao::MCP4725_ALIMENTACAO_MV);
static_assert(configuracao::PEDAL_MARGEM_FALHA_MV <= configuracao::PEDAL_TENSAO_SOLTO_MV &&
              configuracao::PEDAL_TENSAO_SOLTO_MV < configuracao::PEDAL_TENSAO_PRESSIONADO_MV &&
              configuracao::ZONA_MORTA_PEDAL_PADRAO_DECIMOS <= CONTROLE_REMOTO_ZONA_MORTA_MAXIMA);
static_assert(configuracao::FAIXA_LIMITADOR_CENTESIMOS_KMH > 0 &&
              configuracao::MARGEM_RETORNO_CENTESIMOS_KMH > 0 &&
              configuracao::AMOSTRAS_RETORNO_LIMITADOR > 0);

struct LimitadorVelocidade {
    int64_t ultima_amostra_us = 0;
    uint8_t amostras_abaixo = 0;
    bool aguarda_retorno = false;

    uint16_t calcular_teto(uint32_t velocidade, uint32_t limite,
                           uint16_t teto_acelerador, int64_t amostra_us,
                           const ajustes_desenvolvedor_t& config) {
        const bool amostra_nova = amostra_us != ultima_amostra_us;
        ultima_amostra_us = amostra_us;
        if (velocidade >= limite) {
            aguarda_retorno = true;
            amostras_abaixo = 0;
            return 0;
        }

        const uint32_t faixa = std::min(config.faixa_limitador_centesimos,
                                        std::max<uint32_t>(1, limite / 3u));
        const uint32_t margem = std::min(config.margem_limitador_centesimos,
                                         std::max<uint32_t>(1, faixa / 2u));
        const uint32_t folga = limite - velocidade;
        if (aguarda_retorno && amostra_nova) {
            amostras_abaixo = folga >= margem ? amostras_abaixo + 1u : 0;
            aguarda_retorno = amostras_abaixo < config.amostras_retorno_limitador;
        }
        if (aguarda_retorno) return 0;

        return static_cast<uint16_t>(teto_acelerador * std::min(folga, faixa) / faixa);
    }
};

// Preserva frações de décimo de percentual quando uma notificação adianta o ciclo.
uint32_t aplicar_rampa_acelerador(uint32_t anterior, uint16_t alvo, int64_t intervalo_us, uint16_t rampa) {
    const int64_t intervalo_limitado_us = std::clamp<int64_t>(
        intervalo_us, 0, CONTROLE_REMOTO_PERIODO_MS * 1000LL);
    const auto incremento = static_cast<uint32_t>(
        rampa * intervalo_limitado_us / 1000LL);
    return std::min<uint32_t>(alvo * 1000u, anterior + incremento);
}

portMUX_TYPE trava = portMUX_INITIALIZER_UNLOCKED;
constexpr ajustes_desenvolvedor_t AJUSTES_PADRAO{
    configuracao::PEDAL_TENSAO_SOLTO_MV, configuracao::PEDAL_TENSAO_PRESSIONADO_MV,
    configuracao::PEDAL_LIMIAR_SOLTO_MV, configuracao::PEDAL_MARGEM_FALHA_MV,
    configuracao::ZONA_MORTA_PEDAL_PADRAO_DECIMOS, configuracao::DAC_TENSAO_MINIMA_SAIDA_MV,
    configuracao::MCP4725_ALIMENTACAO_MV, configuracao::PEDAL_SOLTO_PARA_ARMAR_MS,
    configuracao::SUBIDA_ACELERADOR_DECIMOS_POR_S, configuracao::FAIXA_LIMITADOR_CENTESIMOS_KMH,
    configuracao::MARGEM_RETORNO_CENTESIMOS_KMH, configuracao::AMOSTRAS_RETORNO_LIMITADOR,
    configuracao::VELOCIDADE_MUDANCA_FAIXA_CENTESIMOS, CONTROLE_REMOTO_REARME_MINIMO_MS,
    configuracao::INTERVALO_ATUALIZACAO_DAC_MS, configuracao::INTERVALO_RESONDAGEM_DAC_MS,
    configuracao::DIAMETRO_RODA_ESQUERDA_M, configuracao::DIAMETRO_RODA_DIREITA_M,
    configuracao::GIROS_ENCODER_ESQUERDO_POR_GIRO_RODA, configuracao::GIROS_ENCODER_DIREITO_POR_GIRO_RODA,
    1.0f, 1.0f, configuracao::PULSOS_ENCODER_ESQUERDO_POR_GIRO, configuracao::PULSOS_ENCODER_DIREITO_POR_GIRO,
    1.0f, 1.0f, 1.0f, 1.0f, configuracao::ACELERADOR_MAXIMO_PADRAO_DECIMOS,
    configuracao::VELOCIDADE_MAXIMA_PADRAO_CENTESIMOS, configuracao::MARCHA_MAXIMA_PADRAO,
    configuracao::SOC_BATERIAS_SERIE, configuracao::SOC_VAZIO_POR_BATERIA_MV,
    configuracao::SOC_CHEIO_POR_BATERIA_MV, configuracao::SOC_REPOUSO_MS,
    configuracao::INTENSIDADE_BUZZER_PERCENTUAL,
    configuracao::LED_INTEGRADO_HABILITADO_PADRAO, 0
};
ajustes_desenvolvedor_t ajustes = AJUSTES_PADRAO;
uint32_t revisao_ajustes = 0;
std::atomic<uint32_t> revisao_geometria_publicada{0};
TaskHandle_t tarefa_controle = nullptr;
SemaphoreHandle_t evento_zero_ota = nullptr;
StaticSemaphore_t memoria_evento_zero_ota{};
SemaphoreHandle_t mutex_limites = nullptr;
StaticSemaphore_t memoria_mutex_limites{};
uint32_t pedido_zero_ota = 0;
uint32_t pedido_zero_ota_confirmado = 0;
bool iniciado = false;
bool observando_firmware_ota = false;
bool limites_em_gravacao = false;
bool revalidar_zero_dac = false;
int64_t rearme_disponivel_em_us = 0;
int64_t parada_em_us = 0;
uint32_t pedal_recebido_em_ms = 0;
bool exige_pedal_zero = true;
uint32_t pedal_zero_desde_ms = 0;
uint8_t marcha_candidata = FAIXA_MARCHA_INDEFINIDA;
uint8_t ultima_marcha_confirmada = FAIXA_MARCHA_INDEFINIDA;
uint32_t marcha_candidata_desde_ms = 0;
bool reversao_candidata = false;
bool seletor_re_estavel = false;
uint32_t reversao_candidata_desde_ms = 0;
uint32_t ultima_sondagem_dac_ms = 0;
uint16_t ultima_saida_dac_mv = UINT16_MAX;
TickType_t ultimo_ciclo_escrita_dac = 0;
uint32_t falhas_pedal = 0;
bool falha_saidas = false;
uint16_t acelerador_aplicado_decimos = 0;
bool rele_energizado = false;
uint32_t ultima_leitura_aceleracao = 0;
uint32_t aceleracao_recebida_em_ms = 0;
LimitadorVelocidade limitador_velocidade{};
uint32_t acelerador_em_milesimos = 0;
int64_t ultimo_ciclo_controle_us = 0;
estado_controle_remoto_t estado = [] {
    estado_controle_remoto_t seguro{};
    seguro.formato = CONTROLE_REMOTO_FORMATO;
    seguro.faixa_marcha_maxima = configuracao::MARCHA_MAXIMA_PADRAO;
    seguro.acelerador_maximo_decimos_percentual = configuracao::ACELERADOR_MAXIMO_PADRAO_DECIMOS;
    seguro.velocidade_maxima_centesimos_kmh = configuracao::VELOCIDADE_MAXIMA_PADRAO_CENTESIMOS;
    seguro.parada_solicitada = configuracao::PARADA_EMERGENCIA_NA_PARTIDA;
    seguro.armamento_inicial_pendente = 1;
    seguro.corte_por_rele_solicitado = 1;
    seguro.estado_operacao = seguro.parada_solicitada ? CONTROLE_ESTADO_PARADO : CONTROLE_ESTADO_BLOQUEADO;
    seguro.reversao_permitida = configuracao::PERMITIR_REVERSAO_PADRAO;
    seguro.zona_morta_decimos_percentual = configuracao::ZONA_MORTA_PEDAL_PADRAO_DECIMOS;
    seguro.tensao_minima_saida_mv = configuracao::DAC_TENSAO_MINIMA_SAIDA_MV;
    return seguro;
}();

uint32_t restante_ms(int64_t prazo_us, int64_t agora_us) {
    const int64_t restante_us = prazo_us - agora_us;
    return restante_us > 0 ? static_cast<uint32_t>((restante_us + 999) / 1000) : 0;
}

class TravaLimites {
public:
    explicit TravaLimites(SemaphoreHandle_t mutex)
        : mutex_(mutex != nullptr && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE ? mutex : nullptr) {}
    ~TravaLimites() { if (mutex_ != nullptr) xSemaphoreGive(mutex_); }
    TravaLimites(const TravaLimites&) = delete;
    TravaLimites& operator=(const TravaLimites&) = delete;
    bool adquirida() const { return mutex_ != nullptr; }

private:
    SemaphoreHandle_t mutex_;
};

bool amostra_pedal_recente(uint32_t agora) {
    return pedal_recebido_em_ms != 0 &&
        agora - pedal_recebido_em_ms <= CONTROLE_REMOTO_EXPIRACAO_PEDAL_MS;
}

bool solicitacao_rearme_valida(int64_t recebido_us, int64_t agora_us) {
    return recebido_us > 0 && recebido_us >= rearme_disponivel_em_us && recebido_us <= agora_us &&
        agora_us - recebido_us <= configuracao::VALIDADE_SOLICITACAO_REARME_MS * 1000LL;
}

int32_t codificar_ajuste(const ajustes_desenvolvedor_t& config, uint8_t id) {
#define CODIFICAR(tipo, nome, escala, minimo, maximo) \
    case AJUSTE_##nome: return static_cast<int32_t>(std::lround(config.nome * escala));
    switch (id) {
        AJUSTES_DESENVOLVEDOR(CODIFICAR)
        default: return 0;
    }
#undef CODIFICAR
}

bool decodificar_ajuste(ajustes_desenvolvedor_t& config, uint8_t id, int32_t valor) {
#define DECODIFICAR(tipo, nome, escala, minimo, maximo) \
    case AJUSTE_##nome: \
        if (valor < minimo || valor > maximo) return false; \
        config.nome = static_cast<tipo>(static_cast<tipo>(valor) / escala); \
        return true;
    switch (id) {
        AJUSTES_DESENVOLVEDOR(DECODIFICAR)
        default: return false;
    }
#undef DECODIFICAR
}

bool calibracao_valida(const ajustes_desenvolvedor_t& config) {
    return config.pedal_margem_mv <= config.pedal_inicio_mv &&
        config.pedal_inicio_mv < config.pedal_fim_mv &&
        config.pedal_solto_mv <= config.pedal_fim_mv &&
        config.pedal_solto_mv > config.pedal_inicio_mv - config.pedal_margem_mv &&
        config.dac_minimo_mv < config.dac_maximo_mv &&
        config.dac_maximo_mv <= configuracao::MCP4725_ALIMENTACAO_MV &&
        config.soc_vazio_mv < config.soc_cheio_mv &&
        static_cast<uint32_t>(config.soc_baterias_serie) * config.soc_cheio_mv <= UINT16_MAX;
}

ajustes_desenvolvedor_t carregar_ajustes() {
    auto restaurados = AJUSTES_PADRAO;
    nvs_handle_t armazenamento;
    const esp_err_t abertura = nvs_open(ESPACO_NVS_CONTROLE, NVS_READONLY, &armazenamento);
    if (abertura != ESP_OK) {
        if (abertura != ESP_ERR_NVS_NOT_FOUND)
            ESP_LOGW(ETIQUETA, "Falha ao ler ajustes da NVS: %s", esp_err_to_name(abertura));
        return restaurados;
    }
    CalibracaoNvs registro{};
    size_t tamanho = sizeof(registro);
    const esp_err_t leitura = nvs_get_blob(armazenamento, CHAVE_CALIBRACAO, &registro, &tamanho);
    if (leitura == ESP_OK || leitura == ESP_ERR_NVS_INVALID_LENGTH) {
        auto calibracao = AJUSTES_PADRAO;
        const bool versao_anterior = registro.versao == VERSAO_CALIBRACAO_NVS_ANTERIOR;
        bool valida = leitura == ESP_OK && tamanho == sizeof(registro) &&
            (versao_anterior || registro.versao == VERSAO_CALIBRACAO_NVS);
        for (uint8_t id = 0; valida && id < AJUSTE_limite_acelerador_decimos; ++id)
            valida = decodificar_ajuste(calibracao, id, registro.valores[id]);
        if (valida && calibracao_valida(calibracao)) {
            // A versão anterior gravava o padrão junto com qualquer ajuste.
            if (versao_anterior && calibracao.atualizacao_dac_ms == INTERVALO_DAC_PADRAO_ANTERIOR_MS)
                calibracao.atualizacao_dac_ms = AJUSTES_PADRAO.atualizacao_dac_ms;
            calibracao.revisao_geometria = 1;
            restaurados = calibracao;
        } else {
            ESP_LOGW(ETIQUETA, "Calibração inválida na NVS; usando valores padrão");
        }
    } else if (leitura != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(ETIQUETA, "Falha ao ler calibração: %s", esp_err_to_name(leitura));
    }
    uint32_t velocidade = 0;
    uint32_t acelerador = 0;
    uint32_t marcha = 0;
    if (nvs_get_u32(armazenamento, CHAVE_VELOCIDADE, &velocidade) == ESP_OK &&
        velocidade <= CONTROLE_REMOTO_VELOCIDADE_MAXIMA)
        restaurados.limite_velocidade_centesimos = velocidade;
    if (nvs_get_u32(armazenamento, CHAVE_ACELERADOR, &acelerador) == ESP_OK &&
        acelerador <= CONTROLE_REMOTO_ACELERADOR_MAXIMO)
        restaurados.limite_acelerador_decimos = static_cast<uint16_t>(acelerador);
    if (nvs_get_u32(armazenamento, CHAVE_MARCHA, &marcha) == ESP_OK &&
        marcha >= FAIXA_MARCHA_L && marcha <= FAIXA_MARCHA_H)
        restaurados.marcha_maxima = static_cast<uint8_t>(marcha);
    resposta_ajustes_desenvolvedor_extras_t extras{};
    tamanho = AJUSTES_EXTRAS_TAMANHO_LEGADO;
    if (nvs_get_blob(armazenamento, CHAVE_CALIBRACAO_EXTRAS, &extras, &tamanho) == ESP_OK &&
        tamanho == AJUSTES_EXTRAS_TAMANHO_LEGADO) {
        auto calibracao = restaurados;
        bool valida = true;
        for (uint8_t id = AJUSTE_soc_baterias_serie; valida && id < AJUSTE_led_integrado_habilitado; ++id)
            valida = decodificar_ajuste(calibracao, id, extras.valores[id - AJUSTE_soc_baterias_serie]);
        if (valida && calibracao_valida(calibracao)) restaurados = calibracao;
        else ESP_LOGW(ETIQUETA, "Ajustes adicionais inválidos na NVS; usando valores padrão");
    }
    uint8_t led_habilitado = 0;
    if (nvs_get_u8(armazenamento, CHAVE_LED_INTEGRADO, &led_habilitado) == ESP_OK)
        (void)decodificar_ajuste(restaurados, AJUSTE_led_integrado_habilitado, led_habilitado);
    nvs_close(armazenamento);
    return restaurados;
}

template <typename Escrever>
esp_err_t salvar_na_nvs(Escrever escrever) {
    nvs_handle_t armazenamento;
    esp_err_t erro = nvs_open(ESPACO_NVS_CONTROLE, NVS_READWRITE,
                             &armazenamento);
    if (erro != ESP_OK) return erro;
    erro = escrever(armazenamento);
    if (erro == ESP_OK) erro = nvs_commit(armazenamento);
    nvs_close(armazenamento);
    return erro;
}

esp_err_t definir_calibracao(uint8_t id, int32_t valor, bool somente_desarmado);

void cancelar_acelerador() {
    estado.acelerador_lido_decimos_percentual = 0;
    estado.acelerador_permitido_decimos_percentual = 0;
    pedal_recebido_em_ms = 0;
    exige_pedal_zero = true;
    pedal_zero_desde_ms = 0;
    limitador_velocidade = {};
    acelerador_em_milesimos = 0;
}

bool ota_bloqueando() {
    return estado.ota_em_andamento || observando_firmware_ota;
}

void desarmar(uint32_t agora);

void informar_falha_pedal(uint32_t agora, esp_err_t erro) {
    portENTER_CRITICAL(&trava);
    const bool pedal_ativo = estado.acelerador_lido_decimos_percentual != 0 ||
        estado.acelerador_permitido_decimos_percentual != 0;
    estado.pedal_disponivel = 0;
    estado.tensao_pedal_mv = 0;
    if (pedal_ativo && !estado.armamento_inicial_pendente) desarmar(agora);
    else cancelar_acelerador();
    portEXIT_CRITICAL(&trava);
    if (++falhas_pedal == 1 || falhas_pedal % 250 == 0)
        ESP_LOGE(ETIQUETA, "Leitura do pedal indisponível: %s (%lu ocorrências)",
                 esp_err_to_name(erro), static_cast<unsigned long>(falhas_pedal));
}

// Corta a tração e exige novo armamento, inclusive em falhas que não são uma emergência comandada.
void desarmar(uint32_t agora) {
    const int64_t inicio_parada_us = esp_timer_get_time();
    cancelar_acelerador();
    estado.faixa_marcha_solicitada = FAIXA_MARCHA_INDEFINIDA;
    estado.reversao_solicitada = 0;
    estado.reversao_acionar_solicitado = 0;
    estado.modo_a_aterrar_solicitado = 0;
    estado.modo_b_aterrar_solicitado = 0;
    estado.armamento_inicial_pendente = 1;
    revalidar_zero_dac = configuracao::HABILITAR_SAIDA_ACELERADOR_DAC;
    if (revalidar_zero_dac) estado.dac_zero_confirmado = 0;
    estado.corte_por_rele_solicitado = 1;
    estado.estado_operacao = estado.parada_solicitada ? CONTROLE_ESTADO_PARADO : CONTROLE_ESTADO_BLOQUEADO;
    parada_em_us = inicio_parada_us;
    rearme_disponivel_em_us = inicio_parada_us + ajustes.espera_rearme_ms * 1000LL;
    ++estado.revisao;
    estado.alterado_em_ms = agora;
}

void solicitar_parada(uint32_t agora) {
    estado.parada_solicitada = 1;
    desarmar(agora);
}

void liberar_parada(uint32_t agora) {
    cancelar_acelerador();
    // Após armar, a tarefa usa a chave atual e exige uma nova leitura de pedal em zero.
    estado.faixa_marcha_solicitada = FAIXA_MARCHA_INDEFINIDA;
    estado.reversao_solicitada = 0;
    estado.parada_solicitada = 0;
    estado.armamento_inicial_pendente = 0;
    revalidar_zero_dac = true;
    estado.dac_zero_confirmado = 0;
    rearme_disponivel_em_us = 0;
    estado.estado_operacao = CONTROLE_ESTADO_BLOQUEADO;
    ++estado.revisao;
    estado.alterado_em_ms = agora;
}

bool aceleracao_valida(const EstadoServicoAceleracao& aceleracao, uint32_t agora) {
    return aceleracao.iniciado && aceleracao.calibrado &&
        aceleracao.eixo_longitudinal_compativel &&
        aceleracao.ultimo_erro == ESP_OK &&
        std::isfinite(aceleracao.aceleracao_longitudinal_ms2) &&
        aceleracao.leituras_ok > 0 &&
        aceleracao_recebida_em_ms != 0 &&
        agora - aceleracao_recebida_em_ms <= configuracao::PRAZO_ACELERACAO_MS;
}

uint32_t velocidade_controle_centesimos(const EstadoOdometro& odometro) {
    // Uma roda que deixa de contar não deve reduzir a velocidade de controle à metade.
    return static_cast<uint32_t>(std::lround(std::max(
        odometro.roda_esquerda.velocidade_kmh, odometro.roda_direita.velocidade_kmh) * 100.0f));
}

void atualizar_logica(int64_t agora_us, const EstadoServicoAceleracao& aceleracao,
                     const EstadoOdometro& odometro, bool simulado,
                     uint8_t marcha_lida, bool re_lida) {
    const uint32_t agora = static_cast<uint32_t>(agora_us / 1000);
    portENTER_CRITICAL(&trava);
    const int64_t intervalo_us = ultimo_ciclo_controle_us == 0 ? 0 : agora_us - ultimo_ciclo_controle_us;
    ultimo_ciclo_controle_us = agora_us;
    const uint8_t bloqueios_anteriores = estado.bloqueios;
    if (aceleracao.leituras_ok != ultima_leitura_aceleracao) {
        ultima_leitura_aceleracao = aceleracao.leituras_ok;
        aceleracao_recebida_em_ms = agora;
    }
    if (std::isfinite(aceleracao.aceleracao_longitudinal_ms2))
        estado.aceleracao_medida_milesimos_ms2 = static_cast<int32_t>(
            std::lround(std::clamp(aceleracao.aceleracao_longitudinal_ms2,
                                   -100.0f, 100.0f) * 1000.0f));
    const bool rodas_validas = servico_odometro_velocidade_valida(odometro, agora_us);
    if (rodas_validas)
        estado.velocidade_medida_centesimos_kmh = velocidade_controle_centesimos(odometro);
    else if (configuracao::HABILITAR_DAC_ISOLADO)
        estado.velocidade_medida_centesimos_kmh = 0;

    if (configuracao::HABILITAR_LEITURA_CHAVE_MARCHA) {
        if (re_lida != reversao_candidata || reversao_candidata_desde_ms == 0) {
            reversao_candidata = re_lida;
            reversao_candidata_desde_ms = agora;
            if (!estado.armamento_inicial_pendente) {
                if ((re_lida && !estado.reversao_permitida) || !rodas_validas ||
                    estado.velocidade_medida_centesimos_kmh >
                        configuracao::VELOCIDADE_MUDANCA_FAIXA_CENTESIMOS ||
                    !amostra_pedal_recente(agora) || estado.tensao_pedal_mv >= ajustes.pedal_solto_mv)
                    desarmar(agora);
                else
                    cancelar_acelerador();
            }
        }
        seletor_re_estavel = agora - reversao_candidata_desde_ms >=
            configuracao::ESTABILIZACAO_CHAVE_MARCHA_MS;
        estado.reversao_solicitada = reversao_candidata;
    }

    if (marcha_lida != marcha_candidata) {
        marcha_candidata = marcha_lida;
        marcha_candidata_desde_ms = agora;
        if (!estado.armamento_inicial_pendente && marcha_lida == FAIXA_MARCHA_INDEFINIDA &&
            ultima_marcha_confirmada != FAIXA_MARCHA_INDEFINIDA)
            desarmar(agora);
        estado.faixa_marcha_fisica = FAIXA_MARCHA_INDEFINIDA;
        estado.faixa_marcha_solicitada = FAIXA_MARCHA_INDEFINIDA;
    } else if (marcha_lida == FAIXA_MARCHA_INDEFINIDA) {
        estado.faixa_marcha_fisica = FAIXA_MARCHA_INDEFINIDA;
        estado.faixa_marcha_solicitada = FAIXA_MARCHA_INDEFINIDA;
        ultima_marcha_confirmada = FAIXA_MARCHA_INDEFINIDA;
    } else if (agora - marcha_candidata_desde_ms >=
               configuracao::ESTABILIZACAO_CHAVE_MARCHA_MS) {
        const uint8_t efetiva = std::min(marcha_lida, estado.faixa_marcha_maxima);
        if (!estado.armamento_inicial_pendente && ultima_marcha_confirmada != FAIXA_MARCHA_INDEFINIDA &&
            ultima_marcha_confirmada != efetiva &&
            (estado.acelerador_lido_decimos_percentual != 0 ||
             estado.velocidade_medida_centesimos_kmh >
                 configuracao::VELOCIDADE_MUDANCA_FAIXA_CENTESIMOS))
            desarmar(agora);
        estado.faixa_marcha_fisica = marcha_lida;
        estado.faixa_marcha_solicitada = estado.armamento_inicial_pendente
            ? static_cast<uint8_t>(FAIXA_MARCHA_INDEFINIDA) : efetiva;
        ultima_marcha_confirmada = efetiva;
    }

    const bool sensores_validos = configuracao::HABILITAR_DAC_ISOLADO ||
        (rodas_validas && aceleracao_valida(aceleracao, agora));
    const bool pedal_recente = amostra_pedal_recente(agora);
    uint8_t bloqueios = 0;
    if (!configuracao::HABILITAR_DAC_ISOLADO && estado.faixa_marcha_solicitada == FAIXA_MARCHA_INDEFINIDA)
        bloqueios |= CONTROLE_BLOQUEIO_SEM_FAIXA;
    if (configuracao::HABILITAR_LEITURA_CHAVE_MARCHA &&
        (!seletor_re_estavel || (estado.reversao_solicitada && !estado.reversao_permitida)))
        bloqueios |= CONTROLE_BLOQUEIO_SEM_FAIXA;
    if (estado.velocidade_maxima_centesimos_kmh == 0)
        bloqueios |= CONTROLE_BLOQUEIO_SEM_LIMITES;
    if (estado.acelerador_maximo_decimos_percentual == 0)
        bloqueios |= CONTROLE_BLOQUEIO_SEM_LIMITES;
    if (!sensores_validos) bloqueios |= CONTROLE_BLOQUEIO_SEM_SENSORES;
    if (estado.armamento_inicial_pendente && rodas_validas &&
        estado.velocidade_medida_centesimos_kmh > ajustes.velocidade_armamento_centesimos)
        bloqueios |= CONTROLE_BLOQUEIO_VELOCIDADE;
    if (simulado) bloqueios |= CONTROLE_BLOQUEIO_TELEMETRIA_SIMULADA;
    if (falha_saidas) bloqueios |= CONTROLE_BLOQUEIO_FALHA_TRAVADA;
    if (!estado.partida_concluida || ota_bloqueando())
        bloqueios |= CONTROLE_BLOQUEIO_PARTIDA_OU_OTA;
    const bool aguardando_pedal_solto = estado.armamento_inicial_pendente &&
        (pedal_zero_desde_ms == 0 || agora - pedal_zero_desde_ms < ajustes.tempo_pedal_solto_ms);
    if (!pedal_recente || exige_pedal_zero || aguardando_pedal_solto)
        bloqueios |= CONTROLE_BLOQUEIO_PEDAL_INDISPONIVEL;
    if (!estado.armamento_inicial_pendente && estado.acelerador_lido_decimos_percentual != 0 &&
        !pedal_recente) {
        desarmar(agora);
        bloqueios |= CONTROLE_BLOQUEIO_PEDAL_INDISPONIVEL;
    } else if (!estado.armamento_inicial_pendente && estado.acelerador_lido_decimos_percentual != 0 &&
               (bloqueios & (CONTROLE_BLOQUEIO_SEM_SENSORES |
                             CONTROLE_BLOQUEIO_TELEMETRIA_SIMULADA))) {
        desarmar(agora);
    }

    estado.bloqueios = bloqueios;
    if (estado.armamento_inicial_pendente || bloqueios != 0 || limites_em_gravacao || revalidar_zero_dac) {
        estado.corte_por_rele_solicitado = 1;
        estado.modo_a_aterrar_solicitado = 0;
        estado.modo_b_aterrar_solicitado = 0;
        estado.reversao_acionar_solicitado = 0;
        estado.acelerador_permitido_decimos_percentual = 0;
        acelerador_em_milesimos = 0;
        limitador_velocidade = {};
        estado.estado_operacao = estado.parada_solicitada
            ? CONTROLE_ESTADO_PARADO : CONTROLE_ESTADO_BLOQUEADO;
    } else {
        estado.corte_por_rele_solicitado = 0;
        const auto marcha = saidas_veiculo_marcha(estado.faixa_marcha_solicitada);
        estado.modo_a_aterrar_solicitado = marcha.modo_a;
        estado.modo_b_aterrar_solicitado = marcha.modo_b;
        estado.reversao_acionar_solicitado = estado.reversao_permitida &&
            estado.reversao_solicitada;
        const uint16_t pedido = std::min(
            estado.acelerador_lido_decimos_percentual,
            estado.acelerador_maximo_decimos_percentual);
        const uint16_t teto_velocidade = limitador_velocidade.calcular_teto(
            estado.velocidade_medida_centesimos_kmh, estado.velocidade_maxima_centesimos_kmh,
            estado.acelerador_maximo_decimos_percentual, odometro.instante_velocidade_us, ajustes);
        acelerador_em_milesimos = aplicar_rampa_acelerador(
            acelerador_em_milesimos, std::min(pedido, teto_velocidade), intervalo_us, ajustes.rampa_decimos_por_s);
        estado.acelerador_permitido_decimos_percentual = acelerador_em_milesimos / 1000u;
        if (teto_velocidade < pedido) {
            estado.estado_operacao = CONTROLE_ESTADO_LIMITANDO_VELOCIDADE;
        } else {
            estado.estado_operacao = pedido == 0 ? CONTROLE_ESTADO_PRONTO : CONTROLE_ESTADO_ACELERANDO;
        }
    }
    portEXIT_CRITICAL(&trava);
    if ((bloqueios & ~bloqueios_anteriores) != 0) {
        ESP_LOGW(ETIQUETA, "Controle bloqueado: bits=0x%02x, velocidade=%.2f km/h, aceleração=%.3f m/s2",
                 bloqueios, odometro.velocidade_kmh,
                 aceleracao.aceleracao_longitudinal_ms2);
    }
}

bool informar_pedal(uint16_t tensao_mv, uint32_t agora) {
    portENTER_CRITICAL(&trava);
    const bool amostra_anterior_recente = amostra_pedal_recente(agora);
    if (tensao_mv < ajustes.pedal_inicio_mv - ajustes.pedal_margem_mv ||
        tensao_mv > ajustes.pedal_fim_mv + ajustes.pedal_margem_mv) {
        portEXIT_CRITICAL(&trava);
        return false;
    }
    const uint32_t curso_mv = ajustes.pedal_fim_mv - ajustes.pedal_inicio_mv;
    const uint32_t inicio_mv = std::max<uint32_t>(ajustes.pedal_solto_mv - 1u,
        ajustes.pedal_inicio_mv + curso_mv * ajustes.zona_morta_decimos / 1000u);
    const uint16_t decimos_percentual = tensao_mv <= inicio_mv ? 0 :
        static_cast<uint16_t>(std::min<float>(CONTROLE_REMOTO_ACELERADOR_MAXIMO,
            (tensao_mv - inicio_mv) * CONTROLE_REMOTO_ACELERADOR_MAXIMO *
                ajustes.ganho_pedal / (ajustes.pedal_fim_mv - inicio_mv)));
    estado.tensao_pedal_mv = tensao_mv;
    estado.pedal_disponivel = 1;
    estado.acelerador_lido_decimos_percentual = decimos_percentual;
    pedal_recebido_em_ms = agora;
    if (tensao_mv < ajustes.pedal_solto_mv && decimos_percentual == 0) {
        if (pedal_zero_desde_ms == 0 || !amostra_anterior_recente)
            pedal_zero_desde_ms = pedal_recebido_em_ms;
        exige_pedal_zero = false;
    } else pedal_zero_desde_ms = 0;
    portEXIT_CRITICAL(&trava);
    return true;
}

bool permite_saida_dac() {
    return (estado.atuacao_habilitada || configuracao::HABILITAR_DAC_ISOLADO) &&
        estado.dac_disponivel && estado.pedal_disponivel &&
        !limites_em_gravacao && !revalidar_zero_dac &&
        !estado.armamento_inicial_pendente && !ota_bloqueando() &&
        !estado.corte_por_rele_solicitado && estado.bloqueios == 0;
}

void registrar_falha_saidas(esp_err_t erro) {
    portENTER_CRITICAL(&trava);
    falha_saidas = true;
    desarmar(tempo_monotonico::agora_ms32());
    portEXIT_CRITICAL(&trava);
    saidas_veiculo_bloquear_falha();
    ESP_LOGE(ETIQUETA, "Falha nas saídas do veículo: %s", esp_err_to_name(erro));
}

void tarefa(void*) {
    const TickType_t periodo = pdMS_TO_TICKS(CONTROLE_REMOTO_PERIODO_MS);
    configASSERT(periodo > 0);
    while (true) {
        const TickType_t inicio_ciclo = xTaskGetTickCount();
        const uint32_t agora = tempo_monotonico::agora_ms32();
        const auto config = ajustes_desenvolvedor_obter();
        if (!mcp4725_disponivel() &&
            (ultima_sondagem_dac_ms == 0 ||
             agora - ultima_sondagem_dac_ms >= config.ressondagem_dac_ms)) {
            ultima_sondagem_dac_ms = agora;
            const esp_err_t erro_dac = mcp4725_iniciar();
            if (erro_dac == ESP_OK) ultima_saida_dac_mv = 0;
            else ESP_LOGW(ETIQUETA, "MCP4725 indisponível: %s",
                          esp_err_to_name(erro_dac));
        }

        uint16_t tensao_pedal_mv = 0;
        const esp_err_t erro_pedal = servico_aquisicao_sensor_ativo(
            SensorControlado::Adc)
            ? leitor_pedal_ler_mv(&tensao_pedal_mv) : ESP_ERR_INVALID_STATE;
        const bool pedal_plausivel = erro_pedal == ESP_OK &&
            informar_pedal(tensao_pedal_mv, tempo_monotonico::agora_ms32());
        if (!pedal_plausivel) {
            informar_falha_pedal(agora, erro_pedal == ESP_OK
                ? ESP_ERR_INVALID_RESPONSE : erro_pedal);
        } else {
            falhas_pedal = 0;
        }
        const auto aceleracao = servico_aceleracao_obter_estado();
        const auto odometro = servico_odometro_obter_estado();
        const bool simulado = servico_simulador_telemetria_esta_ativo();
        uint8_t marcha_lida = FAIXA_MARCHA_INDEFINIDA;
        (void)chave_marcha_ler(&marcha_lida);
        atualizar_logica(esp_timer_get_time(), aceleracao, odometro, simulado, marcha_lida,
                          chave_marcha_re_selecionada());

        const auto controle = servico_controle_remoto_obter_estado();
        bool rele_habilitado = false;
        const esp_err_t erro_saidas = saidas_veiculo_aplicar(controle, rele_habilitado);
        if (erro_saidas != ESP_OK) registrar_falha_saidas(erro_saidas);

        portENTER_CRITICAL(&trava);
        const bool liberar_dac = (rele_habilitado || configuracao::HABILITAR_DAC_ISOLADO) &&
            estado.revisao == controle.revisao && permite_saida_dac();
        const uint16_t acelerador_decimos = liberar_dac ? estado.acelerador_permitido_decimos_percentual : 0;
        const uint16_t minima_mv = estado.tensao_minima_saida_mv;
        const uint16_t maxima_mv = ajustes.dac_maximo_mv;
        const uint16_t saida_alvo_mv = liberar_dac
            ? static_cast<uint16_t>(minima_mv + (maxima_mv - minima_mv) *
                acelerador_decimos / CONTROLE_REMOTO_ACELERADOR_MAXIMO)
            : 0;
        // Inclusive a tensão de repouso invalida a confirmação anterior de zero.
        if (saida_alvo_mv != 0) estado.dac_zero_confirmado = 0;
        const uint32_t pedido = pedido_zero_ota;
        const bool revalidar_zero = revalidar_zero_dac;
        const uint32_t revisao_dac = estado.revisao;
        const bool confirmar_zero_ota = evento_zero_ota != nullptr &&
            estado.ota_em_andamento && pedido != pedido_zero_ota_confirmado;
        portEXIT_CRITICAL(&trava);
        uint16_t saida_mv = saida_alvo_mv;
        const uint32_t intervalo_dac_ms = liberar_dac
            ? config.atualizacao_dac_ms : configuracao::INTERVALO_DAC_SEM_TRACAO_MS;
        // Use o relógio do ciclo para que arredondamentos em ms não pulem uma renovação.
        const TickType_t intervalo_dac = pdMS_TO_TICKS(intervalo_dac_ms + portTICK_PERIOD_MS - 1);
        const bool renovar_dac = configuracao::HABILITAR_SAIDA_ACELERADOR_DAC &&
            inicio_ciclo - ultimo_ciclo_escrita_dac >= intervalo_dac;
        bool zero_escrito_para_ota = false;
        bool zero_escrito = false;
        if (mcp4725_disponivel() &&
            (saida_mv != ultima_saida_dac_mv || renovar_dac || confirmar_zero_ota || revalidar_zero)) {
            esp_err_t erro_dac = mcp4725_definir_milivolts(saida_mv);
            if (erro_dac == ESP_OK && saida_mv != 0) {
                // Uma parada pode abrir o relé enquanto a transação I²C ainda está em curso.
                portENTER_CRITICAL(&trava);
                const bool cancelar = !permite_saida_dac() ||
                    estado.acelerador_permitido_decimos_percentual < acelerador_decimos;
                portEXIT_CRITICAL(&trava);
                if (cancelar) {
                    erro_dac = mcp4725_definir_milivolts(0);
                    if (erro_dac == ESP_OK) saida_mv = 0;
                }
            }
            if (erro_dac == ESP_OK) {
                ultima_saida_dac_mv = saida_mv;
                ultimo_ciclo_escrita_dac = inicio_ciclo;
                zero_escrito = saida_mv == 0;
                zero_escrito_para_ota = confirmar_zero_ota && zero_escrito;
            }
            else {
                portENTER_CRITICAL(&trava);
                if (!estado.armamento_inicial_pendente) desarmar(agora);
                const uint32_t revisao = estado.revisao;
                portEXIT_CRITICAL(&trava);
                const esp_err_t erro_corte = saidas_veiculo_cortar_por_parada(revisao);
                if (erro_corte != ESP_OK) registrar_falha_saidas(erro_corte);
                const bool recuperar_zero = saida_mv != 0 || ultima_saida_dac_mv != 0;
                ultima_saida_dac_mv = UINT16_MAX;
                // Havendo possível tensão, tenta escrever zero já no próximo ciclo.
                ultima_sondagem_dac_ms = recuperar_zero ? 0 : agora;
                ESP_LOGE(ETIQUETA, "Falha na saída do acelerador: %s",
                         esp_err_to_name(erro_dac));
            }
        }
        portENTER_CRITICAL(&trava);
        if (revalidar_zero && zero_escrito && estado.revisao == revisao_dac)
            revalidar_zero_dac = false;
        estado.dac_disponivel = mcp4725_disponivel();
        estado.dac_zero_confirmado = mcp4725_disponivel() &&
            ultima_saida_dac_mv == 0 && !revalidar_zero_dac;
        estado.tensao_dac_mv = ultima_saida_dac_mv == UINT16_MAX
            ? 0 : ultima_saida_dac_mv;
        rele_energizado = rele_habilitado && permite_saida_dac();
        acelerador_aplicado_decimos = estado.dac_disponivel && saida_mv != 0 && ultima_saida_dac_mv == saida_mv &&
            permite_saida_dac() ? acelerador_decimos : 0;
        const bool notificar_zero = zero_escrito_para_ota && estado.ota_em_andamento &&
            pedido == pedido_zero_ota;
        if (notificar_zero) pedido_zero_ota_confirmado = pedido;
        portEXIT_CRITICAL(&trava);
        if (notificar_zero) xSemaphoreGive(evento_zero_ota);
        const TickType_t duracao = xTaskGetTickCount() - inicio_ciclo;
        (void)ulTaskNotifyTake(pdTRUE, duracao < periodo ? periodo - duracao : 1);
    }
}
} // namespace

esp_err_t servico_controle_remoto_iniciar() {
    const esp_err_t erro_chave = chave_marcha_iniciar();
    if (erro_chave != ESP_OK) return erro_chave;
    const esp_err_t erro_saidas = saidas_veiculo_iniciar();
    if (erro_saidas != ESP_OK) return erro_saidas;
    if (configuracao::HABILITAR_SAIDA_ACELERADOR_DAC && evento_zero_ota == nullptr) {
        evento_zero_ota = xSemaphoreCreateBinaryStatic(&memoria_evento_zero_ota);
        if (evento_zero_ota == nullptr) return ESP_ERR_NO_MEM;
    }
    if (mutex_limites == nullptr) mutex_limites = xSemaphoreCreateMutexStatic(&memoria_mutex_limites);
    if (mutex_limites == nullptr) return ESP_ERR_NO_MEM;
    const TravaLimites gravacao(mutex_limites);
    if (!gravacao.adquirida()) return ESP_ERR_INVALID_STATE;
    const auto restaurados = carregar_ajustes();
    portENTER_CRITICAL(&trava);
    if (iniciado) {
        portEXIT_CRITICAL(&trava);
        return ESP_ERR_INVALID_STATE;
    }
    ajustes = restaurados;
    revisao_geometria_publicada.store(ajustes.revisao_geometria, std::memory_order_release);
    estado.zona_morta_decimos_percentual = ajustes.zona_morta_decimos;
    estado.tensao_minima_saida_mv = ajustes.dac_minimo_mv;
    estado.faixa_marcha_maxima = ajustes.marcha_maxima;
    estado.acelerador_maximo_decimos_percentual = ajustes.limite_acelerador_decimos;
    estado.velocidade_maxima_centesimos_kmh = ajustes.limite_velocidade_centesimos;
    if (estado.parada_solicitada)
        rearme_disponivel_em_us = parada_em_us + ajustes.espera_rearme_ms * 1000LL;
    iniciado = true;
    estado.atuacao_habilitada = configuracao::HABILITAR_SAIDA_ACELERADOR_DAC &&
        configuracao::HABILITAR_SAIDAS_VEICULO && configuracao::HABILITAR_LEITURA_CHAVE_MARCHA;
    portEXIT_CRITICAL(&trava);
    if (configuracao::criar_tarefa(tarefa, "controle_remoto", nullptr,
                                   &tarefa_controle) != pdPASS) {
        portENTER_CRITICAL(&trava);
        iniciado = false;
        estado.atuacao_habilitada = 0;
        portEXIT_CRITICAL(&trava);
        ESP_LOGE(ETIQUETA, "Falha ao iniciar tarefa prioritária de controle");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(ETIQUETA, "Pedal ADC no GPIO%d; DAC %s; GPIOs de tração %s",
             configuracao::PINO_ACELERADOR_ADC,
             configuracao::HABILITAR_SAIDA_ACELERADOR_DAC ? "habilitado" : "bloqueado",
             configuracao::HABILITAR_SAIDAS_VEICULO ? "habilitados" : "desabilitados");
    return ESP_OK;
}

void servico_controle_remoto_concluir_partida(bool observando_ota) {
    portENTER_CRITICAL(&trava);
    if (iniciado) {
        if (observando_ota && !observando_firmware_ota)
            solicitar_parada(tempo_monotonico::agora_ms32());
        observando_firmware_ota = observando_ota;
        estado.partida_concluida = 1;
        ++estado.revisao;
        estado.alterado_em_ms = tempo_monotonico::agora_ms32();
    }
    portEXIT_CRITICAL(&trava);
    if (tarefa_controle != nullptr) xTaskNotifyGive(tarefa_controle);
}

esp_err_t servico_controle_remoto_preparar_ota() {
    portENTER_CRITICAL(&trava);
    if (!iniciado || !estado.partida_concluida || ota_bloqueando() || limites_em_gravacao) {
        portEXIT_CRITICAL(&trava);
        return ESP_ERR_INVALID_STATE;
    }
    solicitar_parada(tempo_monotonico::agora_ms32());
    estado.ota_em_andamento = 1;
    estado.dac_zero_confirmado = 0;
    const uint32_t pedido = ++pedido_zero_ota;
    portEXIT_CRITICAL(&trava);
    const esp_err_t erro_saidas = saidas_veiculo_bloquear_ota();
    if (erro_saidas != ESP_OK) return erro_saidas;
    if (tarefa_controle != nullptr) xTaskNotifyGive(tarefa_controle);
    if (!configuracao::HABILITAR_SAIDA_ACELERADOR_DAC) return ESP_OK;
    // O ACK precisa corresponder à escrita realizada para esta solicitação.
    const TickType_t inicio = xTaskGetTickCount();
    const TickType_t prazo = pdMS_TO_TICKS(configuracao::TEMPO_CONFIRMACAO_DAC_OTA_MS);
    while (true) {
        portENTER_CRITICAL(&trava);
        const bool confirmado = pedido_zero_ota_confirmado == pedido;
        portEXIT_CRITICAL(&trava);
        if (confirmado) return ESP_OK;
        const TickType_t decorrido = xTaskGetTickCount() - inicio;
        if (decorrido >= prazo) return ESP_ERR_TIMEOUT;
        (void)xSemaphoreTake(evento_zero_ota, prazo - decorrido);
    }
}

void servico_controle_remoto_encerrar_ota() {
    portENTER_CRITICAL(&trava);
    estado.ota_em_andamento = 0;
    // A parada permanece ativa até o operador rearmar.
    portEXIT_CRITICAL(&trava);
    saidas_veiculo_encerrar_ota();
    if (tarefa_controle != nullptr) xTaskNotifyGive(tarefa_controle);
}

esp_err_t servico_controle_remoto_definir(AjusteControleRemoto ajuste,
                                         uint32_t valor, int64_t recebido_em_us, bool somente_desarmado) {
    switch (ajuste) {
        case AjusteControleRemoto::LimiteMarcha:
            if (valor < FAIXA_MARCHA_L || valor > FAIXA_MARCHA_H)
                return ESP_ERR_INVALID_ARG;
            break;
        case AjusteControleRemoto::VelocidadeMaxima:
            if (valor > CONTROLE_REMOTO_VELOCIDADE_MAXIMA)
                return ESP_ERR_INVALID_ARG;
            break;
        case AjusteControleRemoto::AceleradorMaximo:
            if (valor > CONTROLE_REMOTO_ACELERADOR_MAXIMO)
                return ESP_ERR_INVALID_ARG;
            break;
        case AjusteControleRemoto::ZonaMortaPedal:
            if (valor > CONTROLE_REMOTO_ZONA_MORTA_MAXIMA)
                return ESP_ERR_INVALID_ARG;
            return definir_calibracao(AJUSTE_zona_morta_decimos, valor, somente_desarmado);
        case AjusteControleRemoto::TensaoMinimaDac:
            if (valor > CONTROLE_REMOTO_TENSAO_MINIMA_MAXIMA_MV)
                return ESP_ERR_INVALID_ARG;
            return definir_calibracao(AJUSTE_dac_minimo_mv, valor, somente_desarmado);
        case AjusteControleRemoto::Parada:
            if (valor > 1 && valor != CONTROLE_REMOTO_ARMAR_DAC_ISOLADO) return ESP_ERR_INVALID_ARG;
            break;
        case AjusteControleRemoto::Reversao:
        case AjusteControleRemoto::PermissaoReversao:
            if (valor > 1) return ESP_ERR_INVALID_ARG;
            break;
        default: return ESP_ERR_INVALID_ARG;
    }
    const bool persistir_limite = ajuste == AjusteControleRemoto::VelocidadeMaxima ||
        ajuste == AjusteControleRemoto::AceleradorMaximo || ajuste == AjusteControleRemoto::LimiteMarcha;
    const TravaLimites gravacao(persistir_limite ? mutex_limites : nullptr);
    if (persistir_limite && !gravacao.adquirida()) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&trava);
    const int64_t agora_us = esp_timer_get_time();
    const uint32_t agora = static_cast<uint32_t>(agora_us / 1000);
    const bool parada_imediata = ajuste == AjusteControleRemoto::Parada && valor == 1;
    const bool armar_dac_isolado = configuracao::HABILITAR_DAC_ISOLADO &&
        valor == CONTROLE_REMOTO_ARMAR_DAC_ISOLADO;
    bool parar_ao_bloquear_re = false;
    if ((!iniciado && !parada_imediata) ||
        (somente_desarmado && !estado.armamento_inicial_pendente) ||
        ((somente_desarmado || persistir_limite) && ota_bloqueando())) {
        portEXIT_CRITICAL(&trava);
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t marcha_maxima_anterior = estado.faixa_marcha_maxima;
    const auto rodas_quase_paradas = [] {
        const auto odometro = servico_odometro_obter_estado();
        return servico_odometro_velocidade_valida(odometro, esp_timer_get_time()) &&
            velocidade_controle_centesimos(odometro) <= configuracao::VELOCIDADE_MUDANCA_FAIXA_CENTESIMOS;
    };
    if (ajuste == AjusteControleRemoto::Parada) {
        const auto odometro = parada_imediata ? EstadoOdometro{} : servico_odometro_obter_estado();
        const bool velocidade_valida = servico_odometro_velocidade_valida(odometro, esp_timer_get_time());
        const uint32_t velocidade = velocidade_valida ? velocidade_controle_centesimos(odometro) : 0;
        if (parada_imediata) solicitar_parada(agora);
        // A solicitação deve ter chegado quando a parada atual já aceitava rearme.
        else if (!(armar_dac_isolado || (valor == 0 && estado.atuacao_habilitada)) || !estado.armamento_inicial_pendente ||
                 !solicitacao_rearme_valida(recebido_em_us, agora_us) ||
                 !estado.partida_concluida || ota_bloqueando() || limites_em_gravacao ||
                 falha_saidas || !estado.pedal_disponivel ||
                 exige_pedal_zero || pedal_zero_desde_ms == 0 ||
                 agora - pedal_zero_desde_ms < ajustes.tempo_pedal_solto_ms ||
                 !amostra_pedal_recente(agora) ||
                 estado.acelerador_lido_decimos_percentual != 0 ||
                 (!armar_dac_isolado &&
                    (!velocidade_valida || estado.faixa_marcha_fisica == FAIXA_MARCHA_INDEFINIDA)) ||
                 (configuracao::HABILITAR_LEITURA_CHAVE_MARCHA &&
                  (!seletor_re_estavel || chave_marcha_re_selecionada() != reversao_candidata ||
                   (reversao_candidata && !estado.reversao_permitida))) ||
                 estado.tensao_pedal_mv >= ajustes.pedal_solto_mv ||
                 velocidade > ajustes.velocidade_armamento_centesimos ||
                 estado.velocidade_maxima_centesimos_kmh <= velocidade ||
                 estado.acelerador_maximo_decimos_percentual == 0 ||
                 (estado.bloqueios & (CONTROLE_BLOQUEIO_SEM_SENSORES |
                                      CONTROLE_BLOQUEIO_TELEMETRIA_SIMULADA)) != 0 ||
                 !estado.dac_disponivel || !estado.dac_zero_confirmado) {
            portEXIT_CRITICAL(&trava);
            return ESP_ERR_INVALID_STATE;
        } else liberar_parada(agora);
    } else if (ajuste == AjusteControleRemoto::AceleradorMaximo) {
        if (!estado.armamento_inicial_pendente && valor > estado.acelerador_maximo_decimos_percentual &&
            estado.acelerador_lido_decimos_percentual != 0) {
            portEXIT_CRITICAL(&trava);
            return ESP_ERR_INVALID_STATE;
        }
        estado.acelerador_maximo_decimos_percentual = static_cast<uint16_t>(valor);
        if (estado.acelerador_permitido_decimos_percentual > valor)
            estado.acelerador_permitido_decimos_percentual = static_cast<uint16_t>(valor);
        if (valor == 0) {
            cancelar_acelerador();
            estado.corte_por_rele_solicitado = 1;
        }
    } else if (ajuste == AjusteControleRemoto::PermissaoReversao) {
        estado.reversao_permitida = static_cast<uint8_t>(valor);
        parar_ao_bloquear_re = valor == 0 &&
            (estado.reversao_solicitada || estado.reversao_acionar_solicitado);
        if (parar_ao_bloquear_re) desarmar(agora);
    } else if (ajuste == AjusteControleRemoto::Reversao) {
        if (configuracao::HABILITAR_LEITURA_CHAVE_MARCHA) {
            portEXIT_CRITICAL(&trava);
            return ESP_ERR_NOT_SUPPORTED;
        }
        if ((valor != 0 && !estado.reversao_permitida) || estado.armamento_inicial_pendente ||
            estado.acelerador_lido_decimos_percentual != 0 ||
            estado.acelerador_permitido_decimos_percentual != 0 ||
            (estado.bloqueios & CONTROLE_BLOQUEIO_SEM_SENSORES) != 0 ||
            !rodas_quase_paradas()) {
            portEXIT_CRITICAL(&trava);
            return ESP_ERR_INVALID_STATE;
        }
        estado.reversao_solicitada = static_cast<uint8_t>(valor);
        cancelar_acelerador();
        estado.corte_por_rele_solicitado = 1;
    } else if (ajuste == AjusteControleRemoto::LimiteMarcha) {
        const uint8_t efetiva = estado.faixa_marcha_fisica == FAIXA_MARCHA_INDEFINIDA
            ? static_cast<uint8_t>(FAIXA_MARCHA_INDEFINIDA)
            : std::min<uint8_t>(estado.faixa_marcha_fisica,
                                static_cast<uint8_t>(valor));
        if (!estado.armamento_inicial_pendente && efetiva != estado.faixa_marcha_solicitada &&
            (estado.acelerador_lido_decimos_percentual != 0 ||
             !rodas_quase_paradas())) {
            portEXIT_CRITICAL(&trava);
            return ESP_ERR_INVALID_STATE;
        }
        estado.faixa_marcha_maxima = static_cast<uint8_t>(valor);
        if (!estado.armamento_inicial_pendente && estado.faixa_marcha_solicitada != efetiva) {
            estado.faixa_marcha_solicitada = efetiva;
            ultima_marcha_confirmada = efetiva;
            cancelar_acelerador();
            estado.corte_por_rele_solicitado = 1;
            estado.modo_a_aterrar_solicitado = 0;
            estado.modo_b_aterrar_solicitado = 0;
            estado.reversao_acionar_solicitado = 0;
        }
    } else if (ajuste == AjusteControleRemoto::VelocidadeMaxima) {
        estado.velocidade_maxima_centesimos_kmh = valor;
        cancelar_acelerador();
        estado.corte_por_rele_solicitado = 1;
    }
    if (persistir_limite) limites_em_gravacao = true;
    if (ajuste != AjusteControleRemoto::Parada) ++revisao_ajustes;
    ++estado.revisao;
    estado.alterado_em_ms = agora;
    const uint32_t revisao = estado.revisao;
    const bool saidas_iniciadas = iniciado;
    portEXIT_CRITICAL(&trava);
    if (parada_imediata) {
        const esp_err_t erro = saidas_iniciadas ? saidas_veiculo_cortar_por_parada(revisao) : ESP_OK;
        if (erro != ESP_OK) registrar_falha_saidas(erro);
        if (tarefa_controle != nullptr) xTaskNotifyGive(tarefa_controle);
        return erro;
    }
    if (persistir_limite && tarefa_controle != nullptr) xTaskNotifyGive(tarefa_controle);
    if (ajuste == AjusteControleRemoto::PermissaoReversao) {
        const esp_err_t erro_saida = saidas_veiculo_definir_permissao_reversao(
            valor != 0, parar_ao_bloquear_re, revisao);
        if (erro_saida != ESP_OK) {
            portENTER_CRITICAL(&trava);
            estado.reversao_permitida = 0;
            portEXIT_CRITICAL(&trava);
            registrar_falha_saidas(erro_saida);
            if (tarefa_controle != nullptr) xTaskNotifyGive(tarefa_controle);
            return erro_saida;
        }
    }
    if (persistir_limite) {
        const char* chave = CHAVE_MARCHA;
        if (ajuste == AjusteControleRemoto::VelocidadeMaxima) chave = CHAVE_VELOCIDADE;
        if (ajuste == AjusteControleRemoto::AceleradorMaximo) chave = CHAVE_ACELERADOR;
        const esp_err_t erro_nvs = salvar_na_nvs([chave, valor](nvs_handle_t armazenamento) {
            return nvs_set_u32(armazenamento, chave, valor);
        });
        portENTER_CRITICAL(&trava);
        limites_em_gravacao = false;
        if (erro_nvs != ESP_OK) {
            // Não deixar o limite alterado apenas em RAM após falha ao salvar.
            if (ajuste == AjusteControleRemoto::VelocidadeMaxima)
                estado.velocidade_maxima_centesimos_kmh = 0;
            else if (ajuste == AjusteControleRemoto::AceleradorMaximo)
                estado.acelerador_maximo_decimos_percentual = 0;
            else estado.faixa_marcha_maxima = marcha_maxima_anterior;
            desarmar(tempo_monotonico::agora_ms32());
            const uint32_t revisao_parada = estado.revisao;
            portEXIT_CRITICAL(&trava);
            const esp_err_t erro_corte = saidas_veiculo_cortar_por_parada(revisao_parada);
            if (erro_corte != ESP_OK) registrar_falha_saidas(erro_corte);
            ESP_LOGE(ETIQUETA, "Falha ao salvar limite: %s",
                     esp_err_to_name(erro_nvs));
            return erro_nvs;
        }
        portEXIT_CRITICAL(&trava);
    }
    if (tarefa_controle != nullptr) xTaskNotifyGive(tarefa_controle);
    ESP_LOGI(ETIQUETA, "Controle: ajuste=%u valor=%lu revisão=%lu",
             static_cast<unsigned>(ajuste), static_cast<unsigned long>(valor),
             static_cast<unsigned long>(revisao));
    return ESP_OK;
}

estado_controle_remoto_t servico_controle_remoto_obter_estado() {
    portENTER_CRITICAL(&trava);
    auto copia = estado;
    const int64_t agora_us = esp_timer_get_time();
    const uint32_t agora = static_cast<uint32_t>(agora_us / 1000);
    copia.dac_isolado_habilitado = iniciado && configuracao::HABILITAR_DAC_ISOLADO;
    copia.ota_em_andamento = ota_bloqueando();
    copia.rearme_restante_ms = restante_ms(rearme_disponivel_em_us, agora_us);
    copia.amostra_pedal_restante_ms = pedal_recebido_em_ms != 0 &&
        agora - pedal_recebido_em_ms < CONTROLE_REMOTO_EXPIRACAO_PEDAL_MS
        ? CONTROLE_REMOTO_EXPIRACAO_PEDAL_MS - (agora - pedal_recebido_em_ms) : 0;
    portEXIT_CRITICAL(&trava);
    return copia;
}

SaidaControleVeiculo servico_controle_remoto_obter_saida_aplicada() {
    portENTER_CRITICAL(&trava);
    SaidaControleVeiculo saida{};
    if (permite_saida_dac()) {
        saida.acelerador_decimos = acelerador_aplicado_decimos;
        saida.rele_energizado = rele_energizado;
        saida.reversao = rele_energizado && estado.reversao_acionar_solicitado;
    }
    portEXIT_CRITICAL(&trava);
    return saida;
}

ajustes_desenvolvedor_t ajustes_desenvolvedor_obter() {
    portENTER_CRITICAL(&trava);
    auto copia = ajustes;
    copia.limite_acelerador_decimos = estado.acelerador_maximo_decimos_percentual;
    copia.limite_velocidade_centesimos = estado.velocidade_maxima_centesimos_kmh;
    copia.marcha_maxima = estado.faixa_marcha_maxima;
    portEXIT_CRITICAL(&trava);
    return copia;
}

bool ajustes_desenvolvedor_led_integrado_ativo() {
    portENTER_CRITICAL(&trava);
    const bool ativo = iniciado && ajustes.led_integrado_habilitado;
    portEXIT_CRITICAL(&trava);
    return ativo;
}

uint32_t ajustes_desenvolvedor_revisao_geometria() {
    return revisao_geometria_publicada.load(std::memory_order_acquire);
}

resposta_ajustes_desenvolvedor_t ajustes_desenvolvedor_consultar() {
    resposta_ajustes_desenvolvedor_t resposta{};
    portENTER_CRITICAL(&trava);
    auto copia = ajustes;
    copia.limite_acelerador_decimos = estado.acelerador_maximo_decimos_percentual;
    copia.limite_velocidade_centesimos = estado.velocidade_maxima_centesimos_kmh;
    copia.marcha_maxima = estado.faixa_marcha_maxima;
    resposta.formato = 1;
    resposta.revisao = static_cast<uint8_t>(revisao_ajustes);
    resposta.edicao_permitida = iniciado && estado.armamento_inicial_pendente &&
        !ota_bloqueando() && !limites_em_gravacao;
    portEXIT_CRITICAL(&trava);
    for (uint8_t id = 0; id < AJUSTE_soc_baterias_serie; ++id)
        resposta.valores[id] = codificar_ajuste(copia, id);
    return resposta;
}

resposta_ajustes_desenvolvedor_extras_t ajustes_desenvolvedor_consultar_extras() {
    const auto copia = ajustes_desenvolvedor_obter();
    resposta_ajustes_desenvolvedor_extras_t resposta{};
    for (uint8_t id = AJUSTE_soc_baterias_serie; id < AJUSTES_DESENVOLVEDOR_TOTAL; ++id)
        resposta.valores[id - AJUSTE_soc_baterias_serie] = codificar_ajuste(copia, id);
    return resposta;
}

esp_err_t ajustes_desenvolvedor_definir(uint8_t id, int32_t valor) {
    switch (id) {
        case AJUSTE_limite_acelerador_decimos:
            return servico_controle_remoto_definir(AjusteControleRemoto::AceleradorMaximo, valor, 0, true);
        case AJUSTE_limite_velocidade_centesimos:
            return servico_controle_remoto_definir(AjusteControleRemoto::VelocidadeMaxima, valor, 0, true);
        case AJUSTE_marcha_maxima:
            return servico_controle_remoto_definir(AjusteControleRemoto::LimiteMarcha, valor, 0, true);
        default:
            return definir_calibracao(id, valor, true);
    }
}

namespace {
esp_err_t definir_calibracao(uint8_t id, int32_t valor, bool somente_desarmado) {
    if (id >= AJUSTES_DESENVOLVEDOR_TOTAL ||
        (id >= AJUSTE_limite_acelerador_decimos && id < AJUSTE_soc_baterias_serie))
        return ESP_ERR_INVALID_ARG;
    const TravaLimites gravacao(mutex_limites);
    if (!gravacao.adquirida()) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&trava);
    const bool pedal_ativo = estado.acelerador_lido_decimos_percentual != 0 ||
        estado.acelerador_permitido_decimos_percentual != 0;
    if (!iniciado || ota_bloqueando() ||
        (!estado.armamento_inicial_pendente && (somente_desarmado || pedal_ativo))) {
        portEXIT_CRITICAL(&trava);
        return ESP_ERR_INVALID_STATE;
    }
    auto novos = ajustes;
    if (!decodificar_ajuste(novos, id, valor) || !calibracao_valida(novos)) {
        portEXIT_CRITICAL(&trava);
        return ESP_ERR_INVALID_ARG;
    }
    const bool geometria = id >= AJUSTE_diametro_roda_esquerda_m &&
        id <= AJUSTE_pulsos_encoder_direito_por_giro && codificar_ajuste(ajustes, id) != valor;
    limites_em_gravacao = true;
    portEXIT_CRITICAL(&trava);
    if (tarefa_controle != nullptr) xTaskNotifyGive(tarefa_controle);

    const esp_err_t erro_nvs = salvar_na_nvs([&novos, id](nvs_handle_t armazenamento) {
        if (id == AJUSTE_led_integrado_habilitado)
            return nvs_set_u8(armazenamento, CHAVE_LED_INTEGRADO, novos.led_integrado_habilitado);
        if (id >= AJUSTE_soc_baterias_serie) {
            resposta_ajustes_desenvolvedor_extras_t registro{};
            for (uint8_t campo = AJUSTE_soc_baterias_serie; campo < AJUSTE_led_integrado_habilitado; ++campo)
                registro.valores[campo - AJUSTE_soc_baterias_serie] = codificar_ajuste(novos, campo);
            return nvs_set_blob(armazenamento, CHAVE_CALIBRACAO_EXTRAS, &registro, AJUSTES_EXTRAS_TAMANHO_LEGADO);
        }
        CalibracaoNvs registro{VERSAO_CALIBRACAO_NVS, {}};
        for (uint8_t campo = 0; campo < AJUSTE_limite_acelerador_decimos; ++campo)
            registro.valores[campo] = codificar_ajuste(novos, campo);
        return nvs_set_blob(armazenamento, CHAVE_CALIBRACAO, &registro, sizeof(registro));
    });

    portENTER_CRITICAL(&trava);
    limites_em_gravacao = false;
    if (erro_nvs != ESP_OK) {
        portEXIT_CRITICAL(&trava);
        ESP_LOGE(ETIQUETA, "Falha ao salvar calibração: %s", esp_err_to_name(erro_nvs));
        if (tarefa_controle != nullptr) xTaskNotifyGive(tarefa_controle);
        return erro_nvs;
    }
    if (geometria) ++novos.revisao_geometria;
    ajustes = novos;
    if (geometria) revisao_geometria_publicada.store(novos.revisao_geometria, std::memory_order_release);
    estado.zona_morta_decimos_percentual = ajustes.zona_morta_decimos;
    estado.tensao_minima_saida_mv = ajustes.dac_minimo_mv;
    if (parada_em_us != 0 && estado.armamento_inicial_pendente)
        rearme_disponivel_em_us = std::max(rearme_disponivel_em_us, parada_em_us + ajustes.espera_rearme_ms * 1000LL);
    cancelar_acelerador();
    ++revisao_ajustes;
    ++estado.revisao;
    estado.alterado_em_ms = tempo_monotonico::agora_ms32();
    portEXIT_CRITICAL(&trava);
    if (tarefa_controle != nullptr) xTaskNotifyGive(tarefa_controle);
    return ESP_OK;
}
} // namespace
