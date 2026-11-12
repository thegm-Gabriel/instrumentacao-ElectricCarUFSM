// Traduz estados do sistema em padrões de LEDs e buzzer.
#include "nucleo/configuracao_tarefas.h"
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
#include "servicos/servico_registro_telemetria.h"
#include "servicos/servico_wifi.h"

namespace {
constexpr char ETIQUETA[] = "sinalizacao";
constexpr TickType_t PASSO_TAREFA = pdMS_TO_TICKS(50);
constexpr uint32_t INTERVALO_AVALIACAO_SERVICOS_MS = 500;
constexpr uint32_t DURACAO_AVISO_TEMPORARIO_MS = 3000;
constexpr uint32_t INTERVALO_ALARME_FALHA_MS = 45000;
constexpr uint32_t INTERVALO_ALARME_OTA_FALHA_MS = 120000;
constexpr uint32_t INTERVALO_AVISOS_SONOROS_MS = 30000;
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

enum class PrioridadeSom : uint8_t {
    Rotina = 1,
    Operacao = 2,
    Aviso = 3,
    Ota = 4,
    FalhaOta = 5,
    FalhaCritica = 6,
};

enum class FaseMelodia : uint8_t {
    IniciarNota,
    Soando,
    Pausa,
};

constexpr NotaMusical NOTAS_INICIALIZACAO[] = {
    {392, 90, 35, 24}, {523, 140, 0, 27},
};
constexpr NotaMusical NOTAS_PRONTO[] = {
    {523, 80, 35, 25}, {659, 80, 35, 27}, {784, 170, 0, 29},
};
constexpr NotaMusical NOTAS_CONFIRMACAO[] = {
    {659, 70, 30, 22}, {784, 110, 0, 24},
};
constexpr NotaMusical NOTAS_AVISO[] = {
    {659, 120, 65, 29}, {523, 180, 0, 30},
};
constexpr NotaMusical NOTAS_FALHA_OTA[] = {
    {523, 140, 70, 31}, {392, 220, 0, 33},
};
constexpr NotaMusical NOTAS_FALHA[] = {
    {440, 140, 55, 35}, {330, 190, 55, 37}, {220, 280, 0, 39},
};
constexpr NotaMusical NOTAS_OTA_DISPONIVEL[] = {
    {659, 120, 50, 27}, {880, 180, 0, 30},
};
constexpr NotaMusical NOTAS_OTA_BAIXANDO[] = {
    {587, 85, 45, 27}, {784, 150, 0, 30},
};
constexpr NotaMusical NOTAS_OTA_CONCLUIDO[] = {
    {523, 80, 35, 27}, {659, 80, 35, 29}, {784, 190, 0, 32},
};
constexpr NotaMusical NOTAS_GOOGLE_INICIANDO[] = {
    {587, 75, 35, 22}, {740, 115, 0, 24},
};
constexpr NotaMusical NOTAS_GOOGLE_CONCLUIDO[] = {
    {659, 80, 35, 25}, {880, 150, 0, 28},
};
constexpr NotaMusical NOTAS_GOOGLE_FALHA[] = {
    {587, 120, 65, 29}, {440, 185, 0, 31},
};

constexpr Melodia MELODIA_INICIALIZACAO{
    NOTAS_INICIALIZACAO, sizeof(NOTAS_INICIALIZACAO) / sizeof(NotaMusical)};
constexpr Melodia MELODIA_PRONTO{NOTAS_PRONTO,
                                 sizeof(NOTAS_PRONTO) / sizeof(NotaMusical)};
constexpr Melodia MELODIA_CONFIRMACAO{
    NOTAS_CONFIRMACAO, sizeof(NOTAS_CONFIRMACAO) / sizeof(NotaMusical)};
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
constexpr Melodia MELODIA_GOOGLE_INICIANDO{
    NOTAS_GOOGLE_INICIANDO, sizeof(NOTAS_GOOGLE_INICIANDO) / sizeof(NotaMusical)};
constexpr Melodia MELODIA_GOOGLE_CONCLUIDO{
    NOTAS_GOOGLE_CONCLUIDO, sizeof(NOTAS_GOOGLE_CONCLUIDO) / sizeof(NotaMusical)};
constexpr Melodia MELODIA_GOOGLE_FALHA{
    NOTAS_GOOGLE_FALHA, sizeof(NOTAS_GOOGLE_FALHA) / sizeof(NotaMusical)};

struct SequenciadorMelodia {
    const Melodia* melodia = nullptr;
    PrioridadeSom prioridade = PrioridadeSom::Rotina;
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
bool som_pronto_pendente = false;
int64_t aviso_temporario_ate_ms = 0;
int64_t proximo_alarme_falha_ms = 0;
int64_t proximo_alarme_ota_falha_ms = 0;
int64_t ultimo_aviso_sonoro_ms = -INTERVALO_AVISOS_SONOROS_MS;
int64_t ultimo_log_erro_hardware_ms = -5000;
uint32_t conclusoes_ota_observadas = 0;
uint32_t sincronizacoes_google_iniciadas_observadas = 0;
uint32_t sincronizacoes_google_finalizadas_observadas = 0;
bool sincronizacao_google_em_andamento = false;

int64_t agora_ms() {
    return esp_timer_get_time() / 1000;
}

void registrar_resultado_hardware(esp_err_t erro) {
    if (erro == ESP_OK) {
        portENTER_CRITICAL(&trava_situacao);
        situacao.ultimo_erro = ESP_OK;
        portEXIT_CRITICAL(&trava_situacao);
        return;
    }
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

bool iniciar_melodia(const Melodia& melodia, PrioridadeSom prioridade,
                    int64_t instante_ms) {
    if (sequenciador.melodia != nullptr &&
        prioridade < sequenciador.prioridade) return false;
    registrar_resultado_hardware(gerenciador_indicadores_silenciar());
    sequenciador.melodia = &melodia;
    sequenciador.prioridade = prioridade;
    sequenciador.indice = 0;
    sequenciador.fase = FaseMelodia::IniciarNota;
    sequenciador.proxima_transicao_ms = instante_ms;
    return true;
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

EstadoLeds leds_para_estado(EstadoSinalizacao estado, int64_t instante_ms) {
    const uint32_t fase_1500 = static_cast<uint32_t>(instante_ms % 1500);
    const uint32_t fase_2000 = static_cast<uint32_t>(instante_ms % 2000);
    const uint32_t fase_3000 = static_cast<uint32_t>(instante_ms % 3000);
    EstadoLeds leds{};
    switch (estado) {
        case EstadoSinalizacao::Inicializando:
            leds.azul = intervalo_ativo(fase_1500, 0, 600);
            break;
        case EstadoSinalizacao::Saudavel:
            leds.verde = true;
            break;
        case EstadoSinalizacao::WifiAtivo:
            leds.verde = true;
            leds.azul = intervalo_ativo(fase_3000, 0, 180);
            break;
        case EstadoSinalizacao::OtaVerificando:
            leds.azul = intervalo_ativo(fase_1500, 0, 750);
            break;
        case EstadoSinalizacao::OtaAguardandoAutorizacao:
            leds.azul = true;
            leds.amarelo = intervalo_ativo(fase_3000, 0, 220) ||
                            intervalo_ativo(fase_3000, 420, 220);
            break;
        case EstadoSinalizacao::OtaBaixando:
            leds.azul = true;
            leds.amarelo = intervalo_ativo(fase_1500, 0, 220);
            break;
        case EstadoSinalizacao::OtaConcluido:
            leds.verde = true;
            leds.azul = intervalo_ativo(fase_3000, 0, 220) ||
                        intervalo_ativo(fase_3000, 420, 220);
            break;
        case EstadoSinalizacao::OtaFalha:
            leds.amarelo = true;
            leds.vermelho = intervalo_ativo(fase_3000, 0, 220);
            break;
        case EstadoSinalizacao::Aviso:
            leds.amarelo = intervalo_ativo(fase_2000, 0, 400);
            break;
        case EstadoSinalizacao::Falha:
            // Vermelho predominante; uma breve pausa mostra que a tarefa segue viva.
            leds.vermelho = !intervalo_ativo(fase_2000, 0, 150);
            break;
        case EstadoSinalizacao::Desligado:
            break;
    }
    // O envio ao Google tem um pulso próprio, mas nunca encobre alertas ou OTA.
    if (sincronizacao_google_em_andamento &&
        (estado == EstadoSinalizacao::Saudavel ||
         estado == EstadoSinalizacao::WifiAtivo)) {
        leds.verde = true;
        leds.azul = intervalo_ativo(fase_1500, 0, 180) ||
                    intervalo_ativo(fase_1500, 360, 180);
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
            iniciar_melodia(MELODIA_INICIALIZACAO, PrioridadeSom::Rotina, instante_ms);
            break;
        case EstadoSinalizacao::Saudavel:
            break;
        case EstadoSinalizacao::WifiAtivo:
        case EstadoSinalizacao::OtaVerificando:
            // Entrada no Wi-Fi e consulta OTA são frequentes: indicação apenas por LED.
            break;
        case EstadoSinalizacao::OtaAguardandoAutorizacao:
            iniciar_melodia(MELODIA_OTA_DISPONIVEL, PrioridadeSom::Ota, instante_ms);
            break;
        case EstadoSinalizacao::OtaBaixando:
            iniciar_melodia(MELODIA_OTA_BAIXANDO, PrioridadeSom::Ota, instante_ms);
            break;
        case EstadoSinalizacao::OtaConcluido:
            iniciar_melodia(MELODIA_OTA_CONCLUIDO, PrioridadeSom::Ota, instante_ms);
            break;
        case EstadoSinalizacao::OtaFalha:
            iniciar_melodia(MELODIA_FALHA_OTA, PrioridadeSom::FalhaOta, instante_ms);
            proximo_alarme_ota_falha_ms =
                instante_ms + INTERVALO_ALARME_OTA_FALHA_MS;
            break;
        case EstadoSinalizacao::Aviso:
            if (instante_ms - ultimo_aviso_sonoro_ms >=
                INTERVALO_AVISOS_SONOROS_MS) {
                if (iniciar_melodia(MELODIA_AVISO, PrioridadeSom::Aviso,
                                    instante_ms)) {
                    ultimo_aviso_sonoro_ms = instante_ms;
                }
            }
            break;
        case EstadoSinalizacao::Falha:
            iniciar_melodia(MELODIA_FALHA, PrioridadeSom::FalhaCritica, instante_ms);
            proximo_alarme_falha_ms = instante_ms + INTERVALO_ALARME_FALHA_MS;
            break;
        default:
            break;
    }
}

void tratar_evento(EventoSinalizacao evento, int64_t instante_ms,
                  bool* confirmacao_pendente) {
    portENTER_CRITICAL(&trava_situacao);
    situacao.eventos_recebidos++;
    portEXIT_CRITICAL(&trava_situacao);
    switch (evento) {
        case EventoSinalizacao::Inicializacao:
            sistema_pronto = false;
            break;
        case EventoSinalizacao::SistemaPronto:
            sistema_pronto = true;
            som_pronto_pendente = true;
            break;
        case EventoSinalizacao::OperacaoConfirmada:
            *confirmacao_pendente = true;
            break;
        case EventoSinalizacao::AvisoTemporario:
            aviso_temporario_ate_ms = instante_ms + DURACAO_AVISO_TEMPORARIO_MS;
            break;
        case EventoSinalizacao::FalhaCritica:
            falha_critica_travada = true;
            break;
        case EventoSinalizacao::LimparFalhaCritica:
            falha_critica_travada = false;
            break;
    }
}

bool som_de_rotina_permitido(EstadoSinalizacao estado) {
    return estado == EstadoSinalizacao::Saudavel ||
           estado == EstadoSinalizacao::WifiAtivo;
}

void detectar_sincronizacao_google(EstadoSinalizacao estado_atual,
                                  int64_t instante_ms) {
    const EstadoRegistroTelemetria registro =
        servico_registro_telemetria_obter_estado();
    sincronizacao_google_em_andamento = registro.sincronizacao_em_andamento;
    if (registro.sincronizacoes_finalizadas !=
        sincronizacoes_google_finalizadas_observadas) {
        sincronizacoes_google_finalizadas_observadas =
            registro.sincronizacoes_finalizadas;
        sincronizacoes_google_iniciadas_observadas =
            registro.sincronizacoes_iniciadas;
        ESP_LOGI(ETIQUETA, "Sincronização Google %s",
                 registro.ultimo_resultado_sincronizacao == ESP_OK
                     ? "concluída" : "falhou");
        const bool sucesso = registro.ultimo_resultado_sincronizacao == ESP_OK;
        if (som_de_rotina_permitido(estado_atual) ||
            (!sucesso && estado_atual == EstadoSinalizacao::Aviso)) {
            iniciar_melodia(sucesso ? MELODIA_GOOGLE_CONCLUIDO
                                    : MELODIA_GOOGLE_FALHA,
                            sucesso ? PrioridadeSom::Operacao
                                    : PrioridadeSom::Aviso,
                            instante_ms);
        }
    } else if (registro.sincronizacoes_iniciadas !=
               sincronizacoes_google_iniciadas_observadas) {
        sincronizacoes_google_iniciadas_observadas =
            registro.sincronizacoes_iniciadas;
        if (som_de_rotina_permitido(estado_atual)) {
            iniciar_melodia(MELODIA_GOOGLE_INICIANDO,
                            PrioridadeSom::Operacao, instante_ms);
        }
    }
}

void detectar_conclusao_ota(EstadoSinalizacao estado_atual,
                            int64_t instante_ms) {
    const SituacaoOta ota = servico_ota_obter_situacao();
    if (ota.conclusoes == conclusoes_ota_observadas) return;
    conclusoes_ota_observadas = ota.conclusoes;
    ESP_LOGI(ETIQUETA, "Conclusão OTA detectada para %s",
             servico_ota_nome_alvo(ota.ultimo_alvo_concluido));
    if (estado_atual != EstadoSinalizacao::Falha &&
        estado_atual != EstadoSinalizacao::OtaFalha &&
        estado_atual != EstadoSinalizacao::OtaConcluido) {
        iniciar_melodia(MELODIA_OTA_CONCLUIDO, PrioridadeSom::Ota, instante_ms);
    }
}

void tarefa_sinalizacao(void*) {
    EstadoSinalizacao estado_anterior = EstadoSinalizacao::Desligado;
    EstadoSinalizacao estado_atual = EstadoSinalizacao::Inicializando;
    EstadoLeds leds_anteriores{};
    bool primeira_escrita = true;
    int64_t proxima_tentativa_leds_ms = 0;
    int64_t proxima_avaliacao_servicos_ms = 0;
    while (true) {
        const int64_t instante_ms = agora_ms();
        EventoSinalizacao evento{};
        bool evento_recebido = false;
        bool servicos_avaliados = false;
        bool confirmacao_pendente = false;
        while (xQueueReceive(fila_eventos, &evento, 0) == pdTRUE) {
            tratar_evento(evento, instante_ms, &confirmacao_pendente);
            evento_recebido = true;
        }

        if (evento_recebido || instante_ms >= proxima_avaliacao_servicos_ms) {
            estado_atual = determinar_estado_automatico();
            proxima_avaliacao_servicos_ms =
                instante_ms + INTERVALO_AVALIACAO_SERVICOS_MS;
            servicos_avaliados = true;
        }
        tratar_transicao(estado_anterior, estado_atual, instante_ms);
        if (som_pronto_pendente && som_de_rotina_permitido(estado_atual)) {
            if (iniciar_melodia(MELODIA_PRONTO, PrioridadeSom::Rotina,
                                instante_ms)) {
                som_pronto_pendente = false;
            }
        }
        if (confirmacao_pendente && som_de_rotina_permitido(estado_atual)) {
            iniciar_melodia(MELODIA_CONFIRMACAO,
                            PrioridadeSom::Operacao, instante_ms);
        }
        if (servicos_avaliados) {
            detectar_conclusao_ota(estado_atual, instante_ms);
            detectar_sincronizacao_google(estado_atual, instante_ms);
        }
        if (estado_atual == EstadoSinalizacao::Falha &&
            sequenciador.melodia == nullptr &&
            instante_ms >= proximo_alarme_falha_ms) {
            iniciar_melodia(MELODIA_FALHA, PrioridadeSom::FalhaCritica, instante_ms);
            proximo_alarme_falha_ms = instante_ms + INTERVALO_ALARME_FALHA_MS;
        }
        if (estado_atual == EstadoSinalizacao::OtaFalha &&
            sequenciador.melodia == nullptr &&
            instante_ms >= proximo_alarme_ota_falha_ms) {
            iniciar_melodia(MELODIA_FALHA_OTA, PrioridadeSom::FalhaOta, instante_ms);
            proximo_alarme_ota_falha_ms =
                instante_ms + INTERVALO_ALARME_OTA_FALHA_MS;
        }

        const EstadoLeds leds = leds_para_estado(estado_atual, instante_ms);
        if (instante_ms >= proxima_tentativa_leds_ms &&
            (primeira_escrita || leds.verde != leds_anteriores.verde ||
             leds.amarelo != leds_anteriores.amarelo ||
             leds.vermelho != leds_anteriores.vermelho ||
             leds.azul != leds_anteriores.azul)) {
            const esp_err_t erro_leds = gerenciador_indicadores_definir_leds(leds);
            registrar_resultado_hardware(erro_leds);
            if (erro_leds == ESP_OK) {
                leds_anteriores = leds;
                primeira_escrita = false;
                proxima_tentativa_leds_ms = 0;
            } else {
                proxima_tentativa_leds_ms = instante_ms + 500;
            }
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
    som_pronto_pendente = false;
    falha_critica_travada = false;
    aviso_temporario_ate_ms = 0;
    proximo_alarme_falha_ms = 0;
    proximo_alarme_ota_falha_ms = 0;
    ultimo_aviso_sonoro_ms = -INTERVALO_AVISOS_SONOROS_MS;
    conclusoes_ota_observadas = servico_ota_obter_situacao().conclusoes;
    const EstadoRegistroTelemetria registro =
        servico_registro_telemetria_obter_estado();
    sincronizacoes_google_iniciadas_observadas = registro.sincronizacoes_iniciadas;
    sincronizacoes_google_finalizadas_observadas = registro.sincronizacoes_finalizadas;
    sincronizacao_google_em_andamento = registro.sincronizacao_em_andamento;
    portENTER_CRITICAL(&trava_situacao);
    situacao = {};
    situacao.iniciado = true;
    situacao.estado = EstadoSinalizacao::Inicializando;
    portEXIT_CRITICAL(&trava_situacao);
    if (configuracao::criar_tarefa(tarefa_sinalizacao, "sinalizacao", nullptr, &tarefa_sinalizacao_handle) != pdPASS) {
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
    const bool urgente = evento == EventoSinalizacao::FalhaCritica ||
                         evento == EventoSinalizacao::LimparFalhaCritica;
    // Mantém a ordem entre ativação e limpeza da falha crítica.
    BaseType_t resultado = xQueueSend(fila_eventos, &evento, 0);
    if (resultado != pdTRUE && urgente) {
        // Reserva espaço para uma mudança crítica mesmo sob rajada de eventos.
        EventoSinalizacao descartado{};
        if (xQueueReceive(fila_eventos, &descartado, 0) == pdTRUE) {
            portENTER_CRITICAL(&trava_situacao);
            situacao.eventos_descartados++;
            portEXIT_CRITICAL(&trava_situacao);
            resultado = xQueueSend(fila_eventos, &evento, 0);
        }
    }
    if (resultado != pdTRUE) {
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
