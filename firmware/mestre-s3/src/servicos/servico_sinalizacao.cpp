#include "servicos/servico_sinalizacao.h"

#include <cstddef>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gerenciadores/gerenciador_indicadores.h"
#include "nucleo/configuracao_placa.h"
#include "servicos/servico_diagnostico.h"
#include "servicos/servico_ota.h"
#include "servicos/servico_wifi.h"

namespace {
constexpr char ETIQUETA[] = "sinalizacao";
constexpr TickType_t PASSO_TAREFA = pdMS_TO_TICKS(50);
constexpr uint32_t INTERVALO_AVALIACAO_SERVICOS_MS = 500;
constexpr uint32_t DURACAO_AVISO_TEMPORARIO_MS = 3000;
constexpr uint32_t INTERVALO_ALARME_FALHA_MS = 8000;
constexpr uint32_t INTERVALO_ALARME_OTA_FALHA_MS = 15000;
constexpr UBaseType_t CAPACIDADE_FILA_EVENTOS = 8;

struct NotaMusical {
    uint16_t frequencia_hz;
    uint16_t duracao_ms;
    uint16_t pausa_apos_ms;
    uint8_t intensidade_percentual;
};

struct Melodia {
    const NotaMusical* notas;
    std::size_t quantidade;
};

enum class FaseMelodia : uint8_t {
    IniciarNota,
    Soando,
    Pausa,
};

constexpr NotaMusical NOTAS_INICIALIZACAO[] = {
    {392, 70, 25, 30}, {523, 70, 25, 34}, {659, 80, 25, 38},
    {784, 160, 0, 42},
};
constexpr NotaMusical NOTAS_PRONTO[] = {
    {523, 70, 20, 34}, {659, 70, 20, 38}, {784, 80, 25, 42},
    {1047, 210, 0, 46},
};
constexpr NotaMusical NOTAS_CONFIRMACAO[] = {
    {988, 55, 15, 32}, {1319, 90, 0, 36},
};
constexpr NotaMusical NOTAS_WIFI_ATIVO[] = {
    {659, 55, 20, 28}, {988, 100, 0, 34},
};
constexpr NotaMusical NOTAS_OTA_VERIFICANDO[] = {
    {523, 55, 20, 30}, {659, 55, 20, 34}, {784, 100, 0, 38},
};
constexpr NotaMusical NOTAS_AVISO[] = {
    {740, 90, 60, 50}, {587, 120, 70, 46}, {740, 100, 0, 52},
};
constexpr NotaMusical NOTAS_FALHA_OTA[] = {
    {440, 110, 45, 55}, {349, 160, 45, 58}, {440, 120, 0, 55},
};
constexpr NotaMusical NOTAS_FALHA[] = {
    {330, 130, 50, 62}, {220, 180, 60, 65}, {330, 130, 50, 62},
    {165, 300, 0, 65},
};
constexpr NotaMusical NOTAS_OTA_DISPONIVEL[] = {
    {784, 90, 35, 38}, {988, 90, 35, 42}, {1319, 120, 50, 46},
    {988, 150, 0, 42},
};
constexpr NotaMusical NOTAS_OTA_BAIXANDO[] = {
    {587, 70, 25, 34}, {784, 70, 25, 38}, {988, 120, 0, 42},
};
constexpr NotaMusical NOTAS_OTA_CONCLUIDO[] = {
    {523, 70, 20, 34}, {659, 70, 20, 38}, {784, 70, 20, 42},
    {1047, 130, 25, 48}, {1319, 240, 0, 52},
};

constexpr Melodia MELODIA_INICIALIZACAO{
    NOTAS_INICIALIZACAO, sizeof(NOTAS_INICIALIZACAO) / sizeof(NotaMusical)};
constexpr Melodia MELODIA_PRONTO{NOTAS_PRONTO,
                                 sizeof(NOTAS_PRONTO) / sizeof(NotaMusical)};
constexpr Melodia MELODIA_CONFIRMACAO{
    NOTAS_CONFIRMACAO, sizeof(NOTAS_CONFIRMACAO) / sizeof(NotaMusical)};
constexpr Melodia MELODIA_WIFI_ATIVO{
    NOTAS_WIFI_ATIVO, sizeof(NOTAS_WIFI_ATIVO) / sizeof(NotaMusical)};
constexpr Melodia MELODIA_OTA_VERIFICANDO{
    NOTAS_OTA_VERIFICANDO,
    sizeof(NOTAS_OTA_VERIFICANDO) / sizeof(NotaMusical)};
constexpr Melodia MELODIA_AVISO{NOTAS_AVISO,
                                sizeof(NOTAS_AVISO) / sizeof(NotaMusical)};
constexpr Melodia MELODIA_FALHA_OTA{
    NOTAS_FALHA_OTA, sizeof(NOTAS_FALHA_OTA) / sizeof(NotaMusical)};
constexpr Melodia MELODIA_FALHA{NOTAS_FALHA,
                                sizeof(NOTAS_FALHA) / sizeof(NotaMusical)};
constexpr Melodia MELODIA_OTA_DISPONIVEL{
    NOTAS_OTA_DISPONIVEL, sizeof(NOTAS_OTA_DISPONIVEL) / sizeof(NotaMusical)};
constexpr Melodia MELODIA_OTA_BAIXANDO{
    NOTAS_OTA_BAIXANDO, sizeof(NOTAS_OTA_BAIXANDO) / sizeof(NotaMusical)};
constexpr Melodia MELODIA_OTA_CONCLUIDO{
    NOTAS_OTA_CONCLUIDO, sizeof(NOTAS_OTA_CONCLUIDO) / sizeof(NotaMusical)};

struct SequenciadorMelodia {
    const Melodia* melodia = nullptr;
    std::size_t indice = 0;
    FaseMelodia fase = FaseMelodia::IniciarNota;
    int64_t proxima_transicao_ms = 0;
};

QueueHandle_t fila_eventos = nullptr;
TaskHandle_t tarefa_sinalizacao_handle = nullptr;
portMUX_TYPE trava_situacao = portMUX_INITIALIZER_UNLOCKED;
SituacaoSinalizacao situacao{};
SequenciadorMelodia sequenciador{};
bool falha_critica_travada = false;
bool sistema_pronto = false;
int64_t aviso_temporario_ate_ms = 0;
int64_t proximo_alarme_falha_ms = 0;
int64_t proximo_alarme_ota_falha_ms = 0;
int64_t ultimo_log_erro_hardware_ms = -5000;

int64_t agora_ms() {
    return esp_timer_get_time() / 1000;
}

void registrar_resultado_hardware(esp_err_t erro) {
    if (erro == ESP_OK) return;
    portENTER_CRITICAL(&trava_situacao);
    situacao.falhas_hardware++;
    situacao.ultimo_erro = erro;
    portEXIT_CRITICAL(&trava_situacao);
    const int64_t instante_ms = agora_ms();
    if (instante_ms - ultimo_log_erro_hardware_ms >= 5000) {
        ESP_LOGE(ETIQUETA, "Falha ao atualizar LEDs ou buzzer: %s",
                 esp_err_to_name(erro));
        ultimo_log_erro_hardware_ms = instante_ms;
    }
}

void atualizar_situacao_publica(EstadoSinalizacao estado) {
    portENTER_CRITICAL(&trava_situacao);
    situacao.estado = estado;
    situacao.sistema_pronto = sistema_pronto;
    situacao.falha_critica_travada = falha_critica_travada;
    situacao.melodia_em_execucao = sequenciador.melodia != nullptr;
    portEXIT_CRITICAL(&trava_situacao);
}

void iniciar_melodia(const Melodia& melodia, int64_t instante_ms) {
    registrar_resultado_hardware(gerenciador_indicadores_silenciar());
    sequenciador.melodia = &melodia;
    sequenciador.indice = 0;
    sequenciador.fase = FaseMelodia::IniciarNota;
    sequenciador.proxima_transicao_ms = instante_ms;
}

void atualizar_melodia(int64_t instante_ms) {
    if (sequenciador.melodia == nullptr ||
        instante_ms < sequenciador.proxima_transicao_ms) {
        return;
    }

    const NotaMusical& nota = sequenciador.melodia->notas[sequenciador.indice];
    switch (sequenciador.fase) {
        case FaseMelodia::IniciarNota: {
            // A configuração da placa continua sendo o limite global de volume.
            const uint8_t intensidade =
                nota.intensidade_percentual <
                        configuracao::INTENSIDADE_BUZZER_PERCENTUAL
                    ? nota.intensidade_percentual
                    : configuracao::INTENSIDADE_BUZZER_PERCENTUAL;
            registrar_resultado_hardware(gerenciador_indicadores_tocar_tom(
                nota.frequencia_hz, intensidade));
            sequenciador.proxima_transicao_ms = instante_ms + nota.duracao_ms;
            sequenciador.fase = FaseMelodia::Soando;
            break;
        }
        case FaseMelodia::Soando:
            registrar_resultado_hardware(gerenciador_indicadores_silenciar());
            sequenciador.proxima_transicao_ms = instante_ms + nota.pausa_apos_ms;
            sequenciador.fase = FaseMelodia::Pausa;
            break;
        case FaseMelodia::Pausa:
            sequenciador.indice++;
            if (sequenciador.indice >= sequenciador.melodia->quantidade) {
                sequenciador = {};
            } else {
                sequenciador.fase = FaseMelodia::IniciarNota;
                sequenciador.proxima_transicao_ms = instante_ms;
            }
            break;
    }
}

EstadoSinalizacao determinar_estado_automatico() {
    if (falha_critica_travada) return EstadoSinalizacao::Falha;
    if (!sistema_pronto) return EstadoSinalizacao::Inicializando;

    const ResumoSaudeSistema saude = servico_diagnostico_obter_resumo();
    if (saude.estado == EstadoSaudeSistema::Falha) {
        return EstadoSinalizacao::Falha;
    }

    const SituacaoOta ota = servico_ota_obter_situacao();
    switch (ota.estado) {
        case EstadoServicoOta::Inicializando:
        case EstadoServicoOta::AguardandoRede:
        case EstadoServicoOta::Verificando:
            return EstadoSinalizacao::OtaVerificando;
        case EstadoServicoOta::AguardandoAutorizacao:
            return EstadoSinalizacao::OtaAguardandoAutorizacao;
        case EstadoServicoOta::Baixando:
            return EstadoSinalizacao::OtaBaixando;
        case EstadoServicoOta::Aplicado:
            return EstadoSinalizacao::OtaConcluido;
        case EstadoServicoOta::Falha:
            return EstadoSinalizacao::OtaFalha;
        default:
            break;
    }

    if (agora_ms() < aviso_temporario_ate_ms ||
        saude.estado == EstadoSaudeSistema::Atencao ||
        saude.estado == EstadoSaudeSistema::Recuperando) {
        return EstadoSinalizacao::Aviso;
    }
    if (servico_wifi_obter_resumo().radio_ativo) return EstadoSinalizacao::WifiAtivo;
    return EstadoSinalizacao::Saudavel;
}

bool intervalo_ativo(uint32_t fase_ms, uint32_t inicio_ms, uint32_t duracao_ms) {
    return fase_ms >= inicio_ms && fase_ms < inicio_ms + duracao_ms;
}

bool pulso_duplo(uint32_t fase_ms) {
    return intervalo_ativo(fase_ms, 0, 100) ||
           intervalo_ativo(fase_ms, 250, 100);
}

bool padrao_sos(int64_t instante_ms) {
    const uint32_t fase = static_cast<uint32_t>(instante_ms % 6000);
    return intervalo_ativo(fase, 0, 150) ||
           intervalo_ativo(fase, 300, 150) ||
           intervalo_ativo(fase, 600, 150) ||
           intervalo_ativo(fase, 1200, 450) ||
           intervalo_ativo(fase, 1800, 450) ||
           intervalo_ativo(fase, 2400, 450) ||
           intervalo_ativo(fase, 3300, 150) ||
           intervalo_ativo(fase, 3600, 150) ||
           intervalo_ativo(fase, 3900, 150);
}

EstadoLeds leds_para_estado(EstadoSinalizacao estado, int64_t instante_ms) {
    const uint32_t fase_1000 = static_cast<uint32_t>(instante_ms % 1000);
    const uint32_t fase_1200 = static_cast<uint32_t>(instante_ms % 1200);
    const uint32_t fase_2000 = static_cast<uint32_t>(instante_ms % 2000);
    const bool fase_rapida = ((instante_ms / 160) % 2) == 0;
    EstadoLeds leds{};
    switch (estado) {
        case EstadoSinalizacao::Inicializando:
            leds.azul = ((instante_ms / 180) % 2) == 0;
            leds.amarelo = intervalo_ativo(fase_2000, 1650, 120);
            break;
        case EstadoSinalizacao::Saudavel:
            leds.verde = true;
            break;
        case EstadoSinalizacao::WifiAtivo:
            leds.verde = true;
            leds.azul = pulso_duplo(fase_1200);
            break;
        case EstadoSinalizacao::OtaVerificando:
            leds.azul = fase_rapida;
            leds.amarelo = !fase_rapida;
            break;
        case EstadoSinalizacao::OtaAguardandoAutorizacao:
            leds.azul = true;
            leds.amarelo = pulso_duplo(fase_1200);
            break;
        case EstadoSinalizacao::OtaBaixando:
            leds.azul = true;
            leds.amarelo = intervalo_ativo(fase_1000, 0, 180) ||
                            intervalo_ativo(fase_1000, 500, 180);
            leds.verde = intervalo_ativo(fase_1000, 250, 180) ||
                         intervalo_ativo(fase_1000, 750, 180);
            break;
        case EstadoSinalizacao::OtaConcluido:
            leds.verde = intervalo_ativo(fase_1200, 0, 350);
            leds.azul = intervalo_ativo(fase_1200, 400, 350);
            leds.amarelo = intervalo_ativo(fase_1200, 800, 350);
            break;
        case EstadoSinalizacao::OtaFalha:
            leds.amarelo = true;
            leds.vermelho = pulso_duplo(fase_2000);
            break;
        case EstadoSinalizacao::Aviso:
            leds.amarelo = pulso_duplo(fase_2000);
            break;
        case EstadoSinalizacao::Falha:
            // O vermelho em SOS diferencia falha crítica dos demais avisos.
            leds.vermelho = padrao_sos(instante_ms);
            break;
        case EstadoSinalizacao::Desligado:
            break;
    }
    return leds;
}

void tratar_transicao(EstadoSinalizacao anterior, EstadoSinalizacao atual,
                      int64_t instante_ms) {
    if (anterior == atual) return;
    ESP_LOGI(ETIQUETA, "Estado visual: %s -> %s",
             servico_sinalizacao_nome_estado(anterior),
             servico_sinalizacao_nome_estado(atual));
    switch (atual) {
        case EstadoSinalizacao::Inicializando:
            iniciar_melodia(MELODIA_INICIALIZACAO, instante_ms);
            break;
        case EstadoSinalizacao::Saudavel:
            if (anterior == EstadoSinalizacao::Inicializando) {
                iniciar_melodia(MELODIA_PRONTO, instante_ms);
            }
            break;
        case EstadoSinalizacao::WifiAtivo:
            iniciar_melodia(MELODIA_WIFI_ATIVO, instante_ms);
            break;
        case EstadoSinalizacao::OtaVerificando:
            iniciar_melodia(MELODIA_OTA_VERIFICANDO, instante_ms);
            break;
        case EstadoSinalizacao::OtaAguardandoAutorizacao:
            iniciar_melodia(MELODIA_OTA_DISPONIVEL, instante_ms);
            break;
        case EstadoSinalizacao::OtaBaixando:
            iniciar_melodia(MELODIA_OTA_BAIXANDO, instante_ms);
            break;
        case EstadoSinalizacao::OtaConcluido:
            iniciar_melodia(MELODIA_OTA_CONCLUIDO, instante_ms);
            break;
        case EstadoSinalizacao::OtaFalha:
            iniciar_melodia(MELODIA_FALHA_OTA, instante_ms);
            proximo_alarme_ota_falha_ms =
                instante_ms + INTERVALO_ALARME_OTA_FALHA_MS;
            break;
        case EstadoSinalizacao::Aviso:
            iniciar_melodia(MELODIA_AVISO, instante_ms);
            break;
        case EstadoSinalizacao::Falha:
            iniciar_melodia(MELODIA_FALHA, instante_ms);
            proximo_alarme_falha_ms = instante_ms + INTERVALO_ALARME_FALHA_MS;
            break;
        default:
            break;
    }
}

void tratar_evento(EventoSinalizacao evento, int64_t instante_ms) {
    portENTER_CRITICAL(&trava_situacao);
    situacao.eventos_recebidos++;
    portEXIT_CRITICAL(&trava_situacao);
    switch (evento) {
        case EventoSinalizacao::Inicializacao:
            sistema_pronto = false;
            iniciar_melodia(MELODIA_INICIALIZACAO, instante_ms);
            break;
        case EventoSinalizacao::SistemaPronto:
            sistema_pronto = true;
            iniciar_melodia(MELODIA_PRONTO, instante_ms);
            break;
        case EventoSinalizacao::OperacaoConfirmada:
            if (!falha_critica_travada) {
                iniciar_melodia(MELODIA_CONFIRMACAO, instante_ms);
            }
            break;
        case EventoSinalizacao::AvisoTemporario:
            aviso_temporario_ate_ms = instante_ms + DURACAO_AVISO_TEMPORARIO_MS;
            iniciar_melodia(MELODIA_AVISO, instante_ms);
            break;
        case EventoSinalizacao::FalhaCritica:
            falha_critica_travada = true;
            iniciar_melodia(MELODIA_FALHA, instante_ms);
            proximo_alarme_falha_ms = instante_ms + INTERVALO_ALARME_FALHA_MS;
            break;
        case EventoSinalizacao::LimparFalhaCritica:
            falha_critica_travada = false;
            break;
    }
}

void tarefa_sinalizacao(void*) {
    EstadoSinalizacao estado_anterior = EstadoSinalizacao::Desligado;
    EstadoSinalizacao estado_atual = EstadoSinalizacao::Inicializando;
    EstadoLeds leds_anteriores{};
    bool primeira_escrita = true;
    int64_t proxima_avaliacao_servicos_ms = 0;
    while (true) {
        const int64_t instante_ms = agora_ms();
        EventoSinalizacao evento{};
        bool evento_recebido = false;
        while (xQueueReceive(fila_eventos, &evento, 0) == pdTRUE) {
            tratar_evento(evento, instante_ms);
            evento_recebido = true;
        }

        if (evento_recebido || instante_ms >= proxima_avaliacao_servicos_ms) {
            estado_atual = determinar_estado_automatico();
            proxima_avaliacao_servicos_ms =
                instante_ms + INTERVALO_AVALIACAO_SERVICOS_MS;
        }
        tratar_transicao(estado_anterior, estado_atual, instante_ms);
        if (estado_atual == EstadoSinalizacao::Falha &&
            sequenciador.melodia == nullptr &&
            instante_ms >= proximo_alarme_falha_ms) {
            iniciar_melodia(MELODIA_FALHA, instante_ms);
            proximo_alarme_falha_ms = instante_ms + INTERVALO_ALARME_FALHA_MS;
        }
        if (estado_atual == EstadoSinalizacao::OtaFalha &&
            sequenciador.melodia == nullptr &&
            instante_ms >= proximo_alarme_ota_falha_ms) {
            iniciar_melodia(MELODIA_FALHA_OTA, instante_ms);
            proximo_alarme_ota_falha_ms =
                instante_ms + INTERVALO_ALARME_OTA_FALHA_MS;
        }

        const EstadoLeds leds = leds_para_estado(estado_atual, instante_ms);
        if (primeira_escrita || leds.verde != leds_anteriores.verde ||
            leds.amarelo != leds_anteriores.amarelo ||
            leds.vermelho != leds_anteriores.vermelho ||
            leds.azul != leds_anteriores.azul) {
            registrar_resultado_hardware(
                gerenciador_indicadores_definir_leds(leds));
            leds_anteriores = leds;
            primeira_escrita = false;
        }
        atualizar_melodia(instante_ms);
        atualizar_situacao_publica(estado_atual);
        estado_anterior = estado_atual;
        vTaskDelay(PASSO_TAREFA);
    }
}
}  // namespace

esp_err_t servico_sinalizacao_iniciar() {
    portENTER_CRITICAL(&trava_situacao);
    const bool ja_iniciado = situacao.iniciado;
    portEXIT_CRITICAL(&trava_situacao);
    if (ja_iniciado) return ESP_OK;

    esp_err_t erro = gerenciador_indicadores_iniciar();
    if (erro != ESP_OK) return erro;
    fila_eventos = xQueueCreate(CAPACIDADE_FILA_EVENTOS, sizeof(EventoSinalizacao));
    if (fila_eventos == nullptr) return ESP_ERR_NO_MEM;

    sistema_pronto = false;
    falha_critica_travada = false;
    aviso_temporario_ate_ms = 0;
    proximo_alarme_falha_ms = 0;
    proximo_alarme_ota_falha_ms = 0;
    portENTER_CRITICAL(&trava_situacao);
    situacao = {};
    situacao.iniciado = true;
    situacao.estado = EstadoSinalizacao::Inicializando;
    portEXIT_CRITICAL(&trava_situacao);
    if (xTaskCreate(tarefa_sinalizacao, "sinalizacao", 4096, nullptr, 3,
                    &tarefa_sinalizacao_handle) != pdPASS) {
        vQueueDelete(fila_eventos);
        fila_eventos = nullptr;
        tarefa_sinalizacao_handle = nullptr;
        portENTER_CRITICAL(&trava_situacao);
        situacao = {};
        situacao.ultimo_erro = ESP_ERR_NO_MEM;
        portEXIT_CRITICAL(&trava_situacao);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(ETIQUETA, "Serviço de LEDs e buzzer iniciado sem bloqueio da aplicação");
    return ESP_OK;
}

esp_err_t servico_sinalizacao_notificar(EventoSinalizacao evento) {
    if (fila_eventos == nullptr) return ESP_ERR_INVALID_STATE;
    if (xQueueSend(fila_eventos, &evento, 0) != pdTRUE) {
        portENTER_CRITICAL(&trava_situacao);
        situacao.eventos_descartados++;
        situacao.ultimo_erro = ESP_ERR_TIMEOUT;
        portEXIT_CRITICAL(&trava_situacao);
        ESP_LOGW(ETIQUETA, "Fila de eventos cheia; sinalização %u descartada",
                 static_cast<unsigned>(evento));
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

SituacaoSinalizacao servico_sinalizacao_obter_situacao() {
    portENTER_CRITICAL(&trava_situacao);
    const SituacaoSinalizacao copia = situacao;
    portEXIT_CRITICAL(&trava_situacao);
    return copia;
}

const char* servico_sinalizacao_nome_estado(EstadoSinalizacao estado) {
    switch (estado) {
        case EstadoSinalizacao::Desligado: return "desligado";
        case EstadoSinalizacao::Inicializando: return "inicializando";
        case EstadoSinalizacao::Saudavel: return "saudável";
        case EstadoSinalizacao::WifiAtivo: return "Wi-Fi ativo";
        case EstadoSinalizacao::OtaVerificando: return "verificando OTA";
        case EstadoSinalizacao::OtaAguardandoAutorizacao:
            return "OTA aguardando autorização";
        case EstadoSinalizacao::OtaBaixando: return "instalando OTA";
        case EstadoSinalizacao::OtaConcluido: return "OTA concluído";
        case EstadoSinalizacao::OtaFalha: return "falha OTA";
        case EstadoSinalizacao::Aviso: return "aviso";
        case EstadoSinalizacao::Falha: return "falha";
    }
    return "desconhecido";
}
