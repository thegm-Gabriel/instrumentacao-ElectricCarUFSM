#include "servicos/servico_relatorios.h"

#include <array>

#include "dispositivos/tela_ssd1306.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gerenciadores/gerenciador_i2c.h"
#include "gerenciadores/gerenciador_uart.h"
#include "servicos/servico_comandos.h"
#include "servicos/servico_diagnostico.h"
#include "servicos/servico_ota.h"
#include "servicos/servico_supervisao.h"
#include "servicos/servico_telemetria.h"
#include "servicos/servico_wifi.h"

namespace {
constexpr char ETIQUETA[] = "relatorios";
constexpr TickType_t INTERVALO_AVALIACAO = pdMS_TO_TICKS(100);

struct ConfiguracaoRelatorio {
    const char* nome;
    uint32_t intervalo_ms;
    int64_t ultima_exibicao_us;
};

constexpr uint8_t MASCARA_TODOS_RELATORIOS =
    (1U << static_cast<uint8_t>(GrupoRelatorio::Quantidade)) - 1U;

std::array<ConfiguracaoRelatorio,
           static_cast<size_t>(GrupoRelatorio::Quantidade)>
    configuracoes{{
        {"Telemetria e sensores", 1000, 0},
        {"Comunicação UART", 2000, 0},
        {"Sistema e I2C", 5000, 0},
        {"Atualização OTA", 10000, 0},
    }};

portMUX_TYPE trava_configuracao = portMUX_INITIALIZER_UNLOCKED;
uint8_t relatorios_forcados = 0;
bool relatorios_pausados = false;
portMUX_TYPE trava_dados = portMUX_INITIALIZER_UNLOCKED;
DadosTelemetriaVeiculo dados_mais_recentes{};
bool dados_disponiveis = false;
bool iniciado = false;

void exibir_link(const char* nome, const EstatisticasLinkTelemetria& link) {
    if (!link.respondeu) {
        ESP_LOGW(ETIQUETA, "[LINK %s] sem confirmação recebida | inválidas=%lu",
                 nome, static_cast<unsigned long>(link.confirmacoes_invalidas));
        return;
    }
    ESP_LOGI(ETIQUETA,
             "[LINK %s] confirmações=%lu inválidas=%lu | RTT atual/médio/máximo=%lu/%lu/%lu ms | última resposta há %lu ms",
             nome, static_cast<unsigned long>(link.confirmacoes_recebidas),
             static_cast<unsigned long>(link.confirmacoes_invalidas),
             static_cast<unsigned long>(link.latencia_atual_ms),
             static_cast<unsigned long>(link.latencia_media_ms),
             static_cast<unsigned long>(link.maior_latencia_ms),
             static_cast<unsigned long>(link.idade_ultima_confirmacao_ms));
    ESP_LOGI(ETIQUETA,
             "[MÓDULO %s] tipo=%u | firmware=%u.%u.%u | capacidades=0x%08lX",
             nome, static_cast<unsigned>(link.tipo_satelite),
             static_cast<unsigned>(link.versao_maior),
             static_cast<unsigned>(link.versao_menor),
             static_cast<unsigned>(link.versao_correcao),
             static_cast<unsigned long>(link.capacidades));
}
void exibir_relatorios(const DadosTelemetriaVeiculo& dados) {
    if (servico_relatorios_deve_exibir(GrupoRelatorio::Telemetria)) {
        const EstatisticasTelemetria telemetria =
            servico_telemetria_obter_estatisticas();
        ESP_LOGI(ETIQUETA,
                 "[TELEMETRIA] pacote=%u | %s | marcha=%u velocidade=%.1f km/h carga=%.1f%% | ADC=%u | acc=[%d, %d, %d] giro=[%d, %d, %d]",
                 static_cast<unsigned>(telemetria.ultima_sequencia),
                 dados.simulado ? "SIMULADA" : "REAL",
                 static_cast<unsigned>(dados.marcha), dados.velocidade_kmh,
                 dados.carga_percentual,
                 static_cast<unsigned>(dados.tensao_adc_bruta),
                 dados.aceleracao_x, dados.aceleracao_y, dados.aceleracao_z,
                 dados.giroscopio_x, dados.giroscopio_y, dados.giroscopio_z);
    }

    if (servico_relatorios_deve_exibir(GrupoRelatorio::Uart)) {
        const EstatisticasUart equipe =
            gerenciador_uart_obter_estatisticas(DestinoUart::Equipe);
        const EstatisticasUart visitantes =
            gerenciador_uart_obter_estatisticas(DestinoUart::Visitantes);
        const EstatisticasLinkTelemetria link_equipe =
            servico_telemetria_obter_estatisticas_link(SateliteTelemetria::Equipe);
        const EstatisticasLinkTelemetria link_visitantes =
            servico_telemetria_obter_estatisticas_link(SateliteTelemetria::Visitantes);
        ESP_LOGI(ETIQUETA,
                 "[UART EQUIPE] enviados=%lu (%llu bytes) | fila=%u pico=%u descartes=%lu | erros TX=%lu RX=%lu físicos=%lu",
                 static_cast<unsigned long>(equipe.envios_ok),
                 static_cast<unsigned long long>(equipe.bytes_enviados),
                 static_cast<unsigned>(equipe.ocupacao_fila_tx),
                 static_cast<unsigned>(equipe.maior_ocupacao_fila_tx),
                 static_cast<unsigned long>(equipe.mensagens_descartadas),
                 static_cast<unsigned long>(equipe.erros_envio),
                 static_cast<unsigned long>(equipe.erros_recepcao),
                 static_cast<unsigned long>(equipe.erros_quadro + equipe.erros_paridade));
        ESP_LOGI(ETIQUETA,
                 "[UART VISITANTES] enviados=%lu (%llu bytes) | fila=%u pico=%u descartes=%lu | erros TX=%lu RX=%lu físicos=%lu",
                 static_cast<unsigned long>(visitantes.envios_ok),
                 static_cast<unsigned long long>(visitantes.bytes_enviados),
                 static_cast<unsigned>(visitantes.ocupacao_fila_tx),
                 static_cast<unsigned>(visitantes.maior_ocupacao_fila_tx),
                 static_cast<unsigned long>(visitantes.mensagens_descartadas),
                 static_cast<unsigned long>(visitantes.erros_envio),
                 static_cast<unsigned long>(visitantes.erros_recepcao),
                 static_cast<unsigned long>(visitantes.erros_quadro +
                                            visitantes.erros_paridade));
        exibir_link("EQUIPE", link_equipe);
        exibir_link("VISITANTES", link_visitantes);
    }

    if (servico_relatorios_deve_exibir(GrupoRelatorio::Sistema)) {
        const EstatisticasI2c i2c = gerenciador_i2c_obter_estatisticas();
        const EstatisticasTelemetria telemetria =
            servico_telemetria_obter_estatisticas();
        const EstatisticasSensoresDiagnostico sensores =
            servico_diagnostico_obter_sensores();
        const EstadoTelaSsd1306 tela = tela_ssd1306_obter_estado();
        const ResumoSaudeSistema saude = servico_diagnostico_obter_resumo();
        const ResumoWifi wifi = servico_wifi_obter_resumo();
        const EstatisticasServicoComandos comandos =
            servico_comandos_obter_estatisticas();
        const EstadoModuloSupervisionado supervisao_telemetria =
            servico_supervisao_obter_estado(ModuloSupervisionado::TarefaTelemetria);
        const EstadoModuloSupervisionado supervisao_equipe =
            servico_supervisao_obter_estado(ModuloSupervisionado::UartEquipe);
        const EstadoModuloSupervisionado supervisao_visitantes =
            servico_supervisao_obter_estado(ModuloSupervisionado::UartVisitantes);
        const uint64_t total_segundos = saude.tempo_ativo_ms / 1000u;
        ESP_LOGI(ETIQUETA,
                 "========== SAÚDE DO MESTRE: %s | causas ativas=0x%03lX ==========",
                 servico_diagnostico_nome_estado(saude.estado),
                 static_cast<unsigned long>(saude.causas_ativas));
        ESP_LOGI(ETIQUETA,
                 "[TEMPO] ativo=%lluh %02llum %02llus | último reset=%lu",
                 static_cast<unsigned long long>(total_segundos / 3600u),
                 static_cast<unsigned long long>((total_segundos / 60u) % 60u),
                 static_cast<unsigned long long>(total_segundos % 60u),
                 static_cast<unsigned long>(saude.motivo_ultimo_reset));
        ESP_LOGI(ETIQUETA,
                 "[MEMÓRIA] heap livre=%lu KiB | mínimo=%lu KiB | menor pilha supervisionada=%lu palavras",
                 static_cast<unsigned long>(saude.heap_livre / 1024u),
                 static_cast<unsigned long>(saude.menor_heap_livre / 1024u),
                 static_cast<unsigned long>(saude.palavras_pilha_livres));
        ESP_LOGI(ETIQUETA,
                 "[SENSORES] I2C operações=%lu sondagens=%lu ausentes=%lu erros=%lu último=%s | ADC erros=%lu último=%s | MPU6050 erros=%lu último=%s",
                 static_cast<unsigned long>(i2c.operacoes_ok),
                 static_cast<unsigned long>(i2c.sondagens_ok),
                 static_cast<unsigned long>(i2c.enderecos_ausentes),
                 static_cast<unsigned long>(i2c.erros), esp_err_to_name(i2c.ultimo_erro),
                 static_cast<unsigned long>(sensores.erros_adc),
                 esp_err_to_name(sensores.ultimo_erro_adc),
                 static_cast<unsigned long>(sensores.erros_mpu6050),
                 esp_err_to_name(sensores.ultimo_erro_mpu6050));
        ESP_LOGI(ETIQUETA,
                 "[TELA] SSD1306=%s endereço=0x%02X | quadros=%lu inalterados=%lu erros=%lu último=%s",
                 tela.presente ? "ativa" : "ausente",
                 static_cast<unsigned>(tela.endereco_i2c),
                 static_cast<unsigned long>(tela.quadros_enviados),
                 static_cast<unsigned long>(tela.quadros_inalterados),
                 static_cast<unsigned long>(tela.erros), esp_err_to_name(tela.ultimo_erro));
        ESP_LOGI(ETIQUETA,
                 "[TELEMETRIA] gerados=%lu falhas=%lu pacote=%u CRC=0x%04X | equipe=%s RTT=%lu ms | visitantes=%s RTT=%lu ms",
                 static_cast<unsigned long>(telemetria.pacotes_gerados),
                 static_cast<unsigned long>(telemetria.pacotes_com_falha),
                 static_cast<unsigned>(telemetria.ultima_sequencia),
                 static_cast<unsigned>(telemetria.ultimo_crc16),
                 saude.equipe_respondendo ? "respondendo" : "sem resposta",
                 static_cast<unsigned long>(saude.latencia_equipe_ms),
                 saude.visitantes_respondendo ? "respondendo" : "sem resposta",
                 static_cast<unsigned long>(saude.latencia_visitantes_ms));
        ESP_LOGI(ETIQUETA,
                 "[SUPERVISÃO] telemetria idade=%lu ms falhas/recuperações=%lu/%lu | UART equipe=%lu/%lu | UART visitantes=%lu/%lu",
                 static_cast<unsigned long>(supervisao_telemetria.idade_ultimo_pulso_ms),
                 static_cast<unsigned long>(supervisao_telemetria.falhas_detectadas),
                 static_cast<unsigned long>(supervisao_telemetria.recuperacoes_ok),
                 static_cast<unsigned long>(supervisao_equipe.falhas_detectadas),
                 static_cast<unsigned long>(supervisao_equipe.recuperacoes_ok),
                 static_cast<unsigned long>(supervisao_visitantes.falhas_detectadas),
                 static_cast<unsigned long>(supervisao_visitantes.recuperacoes_ok));
        ESP_LOGI(ETIQUETA,
                 "[COMANDOS] recebidos/enviados=%lu/%lu | consultas TX/RX=%lu/%lu | repetições=%lu timeouts=%lu inválidos=%lu | último=%s",
                 static_cast<unsigned long>(comandos.solicitacoes_recebidas),
                 static_cast<unsigned long>(comandos.respostas_enviadas),
                 static_cast<unsigned long>(comandos.solicitacoes_enviadas),
                 static_cast<unsigned long>(comandos.respostas_recebidas),
                 static_cast<unsigned long>(comandos.repeticoes),
                 static_cast<unsigned long>(comandos.timeouts),
                 static_cast<unsigned long>(comandos.quadros_invalidos),
                 esp_err_to_name(comandos.ultimo_erro));
        ESP_LOGI(ETIQUETA,
                 "[WI-FI] rádio=%s | conexão=%s | rede=%s | RSSI=%d dBm canal=%u | último=%s motivo=%ld",
                 wifi.radio_ativo ? "ativo" : "repouso",
                 wifi.conectado ? "ativa" : "inativa",
                 wifi.rede_ativa[0] == '\0' ? "-" : wifi.rede_ativa,
                 wifi.rssi_dbm, static_cast<unsigned>(wifi.canal),
                 esp_err_to_name(wifi.ultimo_erro),
                 static_cast<long>(wifi.ultimo_motivo_desconexao));
    }

    if (servico_relatorios_deve_exibir(GrupoRelatorio::Ota)) {
        const SituacaoOta ota = servico_ota_obter_situacao();
        ESP_LOGI(ETIQUETA,
                 "[OTA] estado=%s | instalada=%s | disponível=%s | verificações=%lu | intervalo=%lu min | falhas=%lu | último resultado=%s",
                 servico_ota_nome_estado(ota.estado),
                 ota.versao_atual[0] == '\0' ? "-" : ota.versao_atual,
                 ota.versao_disponivel[0] == '\0' ? "nenhuma" : ota.versao_disponivel,
                 static_cast<unsigned long>(ota.verificacoes),
                 static_cast<unsigned long>(ota.intervalo_verificacao_minutos),
                 static_cast<unsigned long>(ota.falhas), esp_err_to_name(ota.ultimo_erro));
        ESP_LOGI(ETIQUETA,
                 "[OTA SAT] equipe=%s (%s -> %s) | visitantes=%s (%s -> %s)",
                 servico_ota_nome_estado_satelite(ota.equipe.estado),
                 ota.equipe.versao_atual[0] ? ota.equipe.versao_atual : "-",
                 ota.equipe.versao_disponivel[0] ? ota.equipe.versao_disponivel : "-",
                 servico_ota_nome_estado_satelite(ota.visitantes.estado),
                 ota.visitantes.versao_atual[0] ? ota.visitantes.versao_atual : "-",
                 ota.visitantes.versao_disponivel[0] ? ota.visitantes.versao_disponivel : "-");
        if (ota.alvo_ativo == AlvoOta::Equipe ||
            ota.alvo_ativo == AlvoOta::Visitantes) {
            const SituacaoOtaSatelite& satelite = ota.alvo_ativo == AlvoOta::Equipe
                                                      ? ota.equipe : ota.visitantes;
            ESP_LOGI(ETIQUETA,
                     "[OTA PROGRESSO] alvo=%s | %lu.%lu%% | %lu/%lu bytes | %.2f MB/s | restante=%lu s",
                     servico_ota_nome_alvo(ota.alvo_ativo),
                     static_cast<unsigned long>(satelite.progresso.percentual_decimos / 10),
                     static_cast<unsigned long>(satelite.progresso.percentual_decimos % 10),
                     static_cast<unsigned long>(satelite.progresso.bytes_transferidos),
                     static_cast<unsigned long>(satelite.progresso.tamanho_total),
                     satelite.progresso.taxa_bytes_por_segundo / (1024.0 * 1024.0),
                     static_cast<unsigned long>(satelite.progresso.tempo_restante_segundos));
        } else if (ota.alvo_ativo == AlvoOta::Mestre &&
                   ota.estado == EstadoServicoOta::Baixando) {
            ESP_LOGI(ETIQUETA,
                     "[OTA PROGRESSO] alvo=mestre | %lu.%lu%% | %lu/%lu bytes | %.2f MB/s | restante=%lu s",
                     static_cast<unsigned long>(ota.progresso.percentual_decimos / 10),
                     static_cast<unsigned long>(ota.progresso.percentual_decimos % 10),
                     static_cast<unsigned long>(ota.progresso.bytes_transferidos),
                     static_cast<unsigned long>(ota.progresso.tamanho_total),
                     ota.progresso.taxa_bytes_por_segundo / (1024.0 * 1024.0),
                     static_cast<unsigned long>(ota.progresso.tempo_restante_segundos));
        }
    }
}

void tarefa_relatorios(void*) {
    while (true) {
        DadosTelemetriaVeiculo copia{};
        bool possui_dados = false;
        portENTER_CRITICAL(&trava_dados);
        copia = dados_mais_recentes;
        possui_dados = dados_disponiveis;
        portEXIT_CRITICAL(&trava_dados);
        if (possui_dados) exibir_relatorios(copia);
        vTaskDelay(INTERVALO_AVALIACAO);
    }
}
}  // namespace

esp_err_t servico_relatorios_iniciar() {
    if (iniciado) return ESP_OK;
    if (xTaskCreate(tarefa_relatorios, "relatorios", 6144, nullptr, 2, nullptr) !=
        pdPASS) {
        ESP_LOGE(ETIQUETA, "Não foi possível iniciar a tarefa de relatórios");
        return ESP_ERR_NO_MEM;
    }
    iniciado = true;
    return ESP_OK;
}

void servico_relatorios_atualizar_dados(
    const DadosTelemetriaVeiculo& dados) {
    portENTER_CRITICAL(&trava_dados);
    dados_mais_recentes = dados;
    dados_disponiveis = true;
    portEXIT_CRITICAL(&trava_dados);
}

const char* servico_relatorios_nome_grupo(GrupoRelatorio grupo) {
    const size_t indice = static_cast<size_t>(grupo);
    return indice < configuracoes.size() ? configuracoes[indice].nome
                                         : "desconhecido";
}

uint32_t servico_relatorios_obter_intervalo(GrupoRelatorio grupo) {
    const size_t indice = static_cast<size_t>(grupo);
    if (indice >= configuracoes.size()) return 0;
    portENTER_CRITICAL(&trava_configuracao);
    const uint32_t intervalo = configuracoes[indice].intervalo_ms;
    portEXIT_CRITICAL(&trava_configuracao);
    return intervalo;
}

esp_err_t servico_relatorios_definir_intervalo(GrupoRelatorio grupo,
                                               uint32_t intervalo_ms) {
    const size_t indice = static_cast<size_t>(grupo);
    if (indice >= configuracoes.size()) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&trava_configuracao);
    configuracoes[indice].intervalo_ms = intervalo_ms;
    configuracoes[indice].ultima_exibicao_us = 0;
    portEXIT_CRITICAL(&trava_configuracao);
    return ESP_OK;
}

bool servico_relatorios_alternar_pausa() {
    portENTER_CRITICAL(&trava_configuracao);
    relatorios_pausados = !relatorios_pausados;
    const bool pausados = relatorios_pausados;
    portEXIT_CRITICAL(&trava_configuracao);
    return pausados;
}

bool servico_relatorios_estao_pausados() {
    portENTER_CRITICAL(&trava_configuracao);
    const bool pausados = relatorios_pausados;
    portEXIT_CRITICAL(&trava_configuracao);
    return pausados;
}

void servico_relatorios_solicitar_resumo() {
    portENTER_CRITICAL(&trava_configuracao);
    relatorios_forcados = MASCARA_TODOS_RELATORIOS;
    portEXIT_CRITICAL(&trava_configuracao);
}

bool servico_relatorios_deve_exibir(GrupoRelatorio grupo) {
    const size_t indice = static_cast<size_t>(grupo);
    if (indice >= configuracoes.size()) return false;

    const int64_t agora_us = esp_timer_get_time();
    bool exibir = false;
    portENTER_CRITICAL(&trava_configuracao);
    const uint8_t mascara = 1U << static_cast<uint8_t>(grupo);
    if ((relatorios_forcados & mascara) != 0) {
        relatorios_forcados &= static_cast<uint8_t>(~mascara);
        exibir = true;
    } else if (!relatorios_pausados && configuracoes[indice].intervalo_ms > 0) {
        const int64_t intervalo_us =
            static_cast<int64_t>(configuracoes[indice].intervalo_ms) * 1000;
        if (configuracoes[indice].ultima_exibicao_us == 0 ||
            agora_us - configuracoes[indice].ultima_exibicao_us >= intervalo_us) {
            exibir = true;
        }
    }
    if (exibir) configuracoes[indice].ultima_exibicao_us = agora_us;
    portEXIT_CRITICAL(&trava_configuracao);
    return exibir;
}
