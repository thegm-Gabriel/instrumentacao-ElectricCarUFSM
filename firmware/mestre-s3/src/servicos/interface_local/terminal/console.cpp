// Implementa o menu serial interativo de operação e diagnóstico.
#include "nucleo/configuracao/tarefas.h"
#include "servicos/interface_local/terminal/console.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nucleo/configuracao/placa.h"
#include "nucleo/configuracao/interface.h"
#include "servicos/veiculo/controle.h"
#include "servicos/manutencao/diagnostico.h"
#include "servicos/manutencao/ota.h"
#include "servicos/historico/registro_sd.h"
#include "servicos/interface_local/relatorios.h"
#include "servicos/comunicacao/wifi_sessoes.h"
#include "servicos/interface_local/terminal/interno.h"

namespace {
using terminal_console::EcoEntrada;
using terminal_console::OpcaoMenu;
using terminal_console::imprimir_opcoes;
using terminal_console::imprimir_linha;
using terminal_console::imprimir_prompt;
using terminal_console::imprimir_titulo;
using terminal_console::solicitar_resumo;
constexpr char ETIQUETA[] = "terminal";
constexpr size_t TAMANHO_MAXIMO_LINHA = 96;
bool iniciado = false;

enum class TelaTerminal : uint8_t {
    Principal,
    Logs,
    Sensores,
    ControleVeiculo,
    ControleZonaMorta,
    ControleTensaoMinima,
    ControleAceleradorMaximo,
    Operacao,
    Intervalos,
    EscolherIntervalo,
    Wifi,
};

using TratadorOpcao = void (*)(unsigned long opcao);

struct RotaTerminal {
    TelaTerminal tela;
    TratadorOpcao tratar;
};

enum class ResultadoLeituraLinha : uint8_t {
    Concluida,
    LongaDemais,
};

constexpr OpcaoMenu OPCOES_PRINCIPAL_MONITORAMENTO[] = {
    {1, "Mostrar resumo completo agora"},
    {11, "Ver estado rápido do sistema"},
    {3, "Configurar intervalos dos relatórios"},
    {4, "Pausar ou retomar relatórios periódicos"},
    {7, "Sensores e diagnóstico"},
    {12, "Controle do veículo e pedal"},
};

constexpr OpcaoMenu OPCOES_PRINCIPAL_CONECTIVIDADE[] = {
    {2, "Atualização OTA"},
    {5, "Redes Wi-Fi"},
    {6, "Consultar satélites"},
    {8, "Enviar telemetria pendente ao Google"},
};

constexpr OpcaoMenu OPCOES_PRINCIPAL_FERRAMENTAS[] = {
    {10, "Avisos, logs e aparência do terminal"},
};

struct FonteLogTerminal {
    const char* nome;
    const char* etiqueta;
    esp_log_level_t nivel;
};

FonteLogTerminal fontes_log[] = {
    {"Wi-Fi", "servico_wifi", ESP_LOG_INFO},
    {"Rádio Wi-Fi", "gerenciador_wifi", ESP_LOG_INFO},
    {"OTA", "servico_ota", ESP_LOG_INFO},
    {"Transporte OTA", "ota_uart", ESP_LOG_INFO},
    {"Google e SD", "registro_telemetria", ESP_LOG_INFO},
    {"Telemetria", "servico_telemetria", ESP_LOG_INFO},
    {"Sensores", "diagnostico", ESP_LOG_INFO},
    {"Comunicação UART", "enlace_satelites", ESP_LOG_INFO},
    {"Comandos", "servico_comandos", ESP_LOG_INFO},
    {"Supervisão", "supervisao", ESP_LOG_INFO},
    {"Sinalização", "sinalizacao", ESP_LOG_INFO},
    {"Relatórios", "relatorios", ESP_LOG_INFO},
    {"Cartão SD", "gerenciador_sd", ESP_LOG_INFO},
    {"Odômetro", "servico_odometro", ESP_LOG_INFO},
    {"Aceleração", "servico_aceleracao", ESP_LOG_INFO},
    {"I²C", "gerenciador_i2c", ESP_LOG_INFO},
    {"Simulador", "simulador_telemetria", ESP_LOG_INFO},
    {"Terminal", "terminal", ESP_LOG_INFO},
};
static_assert(sizeof(fontes_log) / sizeof(fontes_log[0]) == QUANTIDADE_FONTES_LOG_TERMINAL,
              "Cada fonte precisa de um identificador estável");
static_assert(ESP_LOG_NONE == 0 && ESP_LOG_ERROR == 1 && ESP_LOG_WARN == 2 && ESP_LOG_INFO == 3,
              "Os níveis de log devem manter os valores do protocolo");
// Etiquetas do projeto menos usadas no menu individual. Mantê-las aqui garante
// que avisos de sensores, drivers e da inicialização continuem visíveis quando
// as mensagens do ESP-IDF forem silenciadas.
constexpr const char* etiquetas_projeto_adicionais[] = {
    "contador_pulsos", "controle_remoto", "encoder_incremental", "gerenciador_ota",
    "gerenciador_uart", "ina228", "indicadores",
    "leitor_tensao", "mestre", "mpu6050", "painel_local", "tela_ssd1306",
};
std::atomic<SemaphoreHandle_t> mutex_logs{nullptr};
StaticSemaphore_t memoria_mutex_logs{};
std::atomic<bool> terminal_cores{configuracao::TERMINAL_CORES_PADRAO};
esp_log_level_t nivel_global_logs = ESP_LOG_INFO;
bool apenas_avisos_projeto = configuracao::TERMINAL_APENAS_AVISOS_PADRAO;

esp_log_level_t nivel_efetivo_fonte(esp_log_level_t nivel, bool apenas_projeto) {
    return apenas_projeto && nivel > ESP_LOG_WARN ? ESP_LOG_WARN : nivel;
}

void aplicar_niveis_logs() {
    esp_log_level_set("*", apenas_avisos_projeto ? ESP_LOG_NONE : nivel_global_logs);
    for (const char* etiqueta : etiquetas_projeto_adicionais)
        esp_log_level_set(etiqueta, apenas_avisos_projeto ? ESP_LOG_WARN : nivel_global_logs);
    for (const auto& fonte : fontes_log)
        esp_log_level_set(fonte.etiqueta, nivel_efetivo_fonte(fonte.nivel, apenas_avisos_projeto));
}

esp_err_t configurar_logs(AjusteLogsTerminal ajuste, uint8_t alvo, uint8_t valor) {
    if (ajuste == AjusteLogsTerminal::Fonte ? alvo >= QUANTIDADE_FONTES_LOG_TERMINAL : alvo != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (ajuste) {
        case AjusteLogsTerminal::Global:
            if (valor > ESP_LOG_INFO) return ESP_ERR_INVALID_ARG;
            nivel_global_logs = static_cast<esp_log_level_t>(valor);
            apenas_avisos_projeto = false;
            for (auto& fonte : fontes_log) fonte.nivel = nivel_global_logs;
            aplicar_niveis_logs();
            return ESP_OK;
        case AjusteLogsTerminal::ApenasAvisosProjeto:
            if (valor > 1) return ESP_ERR_INVALID_ARG;
            apenas_avisos_projeto = valor == 1;
            aplicar_niveis_logs();
            return ESP_OK;
        case AjusteLogsTerminal::Cores:
            if (valor > 1) return ESP_ERR_INVALID_ARG;
            terminal_cores.store(valor == 1, std::memory_order_relaxed);
            return ESP_OK;
        case AjusteLogsTerminal::Fonte: {
            if (valor > ESP_LOG_INFO) return ESP_ERR_INVALID_ARG;
            auto& fonte = fontes_log[alvo];
            fonte.nivel = static_cast<esp_log_level_t>(valor);
            esp_log_level_set(fonte.etiqueta, nivel_efetivo_fonte(fonte.nivel, apenas_avisos_projeto));
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_ARG;
}

const char* nome_nivel_log(esp_log_level_t nivel) {
    switch (nivel) {
        case ESP_LOG_NONE: return "silenciado";
        case ESP_LOG_ERROR: return "erros";
        case ESP_LOG_WARN: return "avisos";
        default: return "informações";
    }
}

TelaTerminal tela_atual = TelaTerminal::Principal;
GrupoRelatorio grupo_em_edicao = GrupoRelatorio::Telemetria;
int caractere_pendente = EOF;
bool entrada_console_nao_bloqueante = false;

}  // namespace

namespace terminal_console {
void imprimir_linha() {
    std::printf("\r\n============================================================\r\n");
}

void imprimir_titulo(const char* titulo) {
    imprimir_linha();
    const bool cores = terminal_cores.load(std::memory_order_relaxed);
    std::printf("  %s%s%s\r\n", cores ? "\033[1;36m" : "", titulo, cores ? "\033[0m" : "");
    std::printf("============================================================\r\n");
}


void imprimir_prompt(const char* orientacao, const char* rotulo) {
    if (orientacao != nullptr && orientacao[0] != '\0') {
        std::printf("\r\n  %s", orientacao);
    }
    std::printf("\r\n  %s > ", rotulo);
    std::fflush(stdout);
}

void solicitar_resumo() {
    std::printf("\r\nPreparando resumo completo...\r\n");
    std::fflush(stdout);
    const esp_err_t erro =
        servico_relatorios_solicitar_resumo_e_aguardar(3000);
    if (erro != ESP_OK) {
        std::printf("[AVISO] O resumo não terminou no prazo: %s\r\n",
                    esp_err_to_name(erro));
    }
}
}  // namespace terminal_console

namespace {
void mostrar_menu_principal();
void mostrar_menu_controle_veiculo();

void mostrar_menu_logs() {
    imprimir_titulo("NEXUS | AVISOS E TERMINAL");
    EstadoLogsTerminal estado{};
    servico_terminal_obter_logs(&estado);
    std::printf("\r\n  Nível geral: %s | cores: %s | perfil: %s\r\n",
                estado.apenas_avisos_projeto ? "ESP-IDF silenciado" :
                    nome_nivel_log(static_cast<esp_log_level_t>(estado.nivel_global)),
                estado.cores ? "ligadas" : "desligadas",
                estado.apenas_avisos_projeto ? "avisos do NEXUS" : "normal");
    std::printf("  [1] Mostrar informações, avisos e erros\r\n");
    std::printf("  [2] Mostrar só avisos e erros\r\n");
    std::printf("  [3] Mostrar só erros\r\n");
    std::printf("  [4] Silenciar todos os logs\r\n");
    std::printf("  [5] Ligar/desligar cores do menu\r\n");
    std::printf("  [6] Silenciar ESP-IDF; manter avisos e erros do NEXUS\r\n");
    std::printf("  [7] Restaurar nível geral e ajustes dos serviços\r\n");
    std::printf("\r\n  Serviço específico: selecione para alternar o nível\r\n");
    for (size_t i = 0; i < QUANTIDADE_FONTES_LOG_TERMINAL; ++i)
        std::printf("  [%u] %-20s %s\r\n", static_cast<unsigned>(i + 10),
                    fontes_log[i].nome,
                    nome_nivel_log(nivel_efetivo_fonte(
                        static_cast<esp_log_level_t>(estado.niveis_fontes[i]),
                        estado.apenas_avisos_projeto)));
    std::printf("  [0] Voltar ao menu principal\r\n");
    imprimir_prompt("Mudanças temporárias: reiniciar restaura os níveis do firmware.");
}

void tratar_menu_logs(unsigned long opcao) {
    if (opcao == 0) {
        tela_atual = TelaTerminal::Principal;
        mostrar_menu_principal();
        return;
    }
    const SemaphoreHandle_t mutex = mutex_logs.load(std::memory_order_acquire);
    xSemaphoreTake(mutex, portMAX_DELAY);
    esp_err_t erro = ESP_ERR_INVALID_ARG;
    if (opcao >= 1 && opcao <= 4) {
        constexpr uint8_t niveis[] = {ESP_LOG_INFO, ESP_LOG_WARN, ESP_LOG_ERROR, ESP_LOG_NONE};
        erro = configurar_logs(AjusteLogsTerminal::Global, 0, niveis[opcao - 1]);
    } else if (opcao == 5) {
        erro = configurar_logs(AjusteLogsTerminal::Cores, 0,
                               !terminal_cores.load(std::memory_order_relaxed));
    } else if (opcao == 6 || opcao == 7) {
        erro = configurar_logs(AjusteLogsTerminal::ApenasAvisosProjeto, 0, opcao == 6);
    } else if (opcao >= 10 && opcao < 10 + QUANTIDADE_FONTES_LOG_TERMINAL) {
        const uint8_t alvo = static_cast<uint8_t>(opcao - 10);
        const uint8_t nivel = fontes_log[alvo].nivel;
        erro = configurar_logs(AjusteLogsTerminal::Fonte, alvo,
                               nivel == ESP_LOG_NONE ? uint8_t{ESP_LOG_INFO} : nivel - 1);
    }
    xSemaphoreGive(mutex);
    if (erro != ESP_OK) std::printf("\r\nOpção inválida.\r\n");
    mostrar_menu_logs();
}

void mostrar_estado_rapido() {
    const ResumoSaudeSistema saude = servico_diagnostico_obter_resumo();
    const ResumoWifi wifi = servico_wifi_obter_resumo();
    const SituacaoOta ota = servico_ota_obter_situacao();
    const EstadoRegistroTelemetria sd =
        servico_registro_telemetria_obter_estado();
    const char* estado_saude = saude.estado == EstadoSaudeSistema::Saudavel ? "normal" :
        saude.estado == EstadoSaudeSistema::Recuperando ? "recuperando" :
        saude.estado == EstadoSaudeSistema::Atencao ? "degradado" : "falha";
    imprimir_titulo("ESTADO RÁPIDO DO MESTRE");
    std::printf("  Saúde: %s | causas: 0x%08lX | ativo: %llu s\r\n",
                saude.valido ? estado_saude : "ainda sem leitura",
                static_cast<unsigned long>(saude.causas_ativas),
                static_cast<unsigned long long>(saude.tempo_ativo_ms / 1000));
    std::printf("  Memória interna: %lu KiB livres | maior bloco: %lu KiB\r\n",
                static_cast<unsigned long>(saude.heap_interno_livre / 1024),
                static_cast<unsigned long>(saude.maior_bloco_heap_interno / 1024));
    std::printf("  PSRAM: %lu KiB livres | pilha mínima: %lu B (%s)\r\n",
                static_cast<unsigned long>(saude.psram_livre / 1024),
                static_cast<unsigned long>(saude.bytes_pilha_livres),
                saude.tarefa_menor_pilha[0] ? saude.tarefa_menor_pilha : "indisponível");
    std::printf("  Wi-Fi: %s | principal: %s | %u rede(s) salva(s)\r\n",
                wifi.conectado ? "conectado" : wifi.sessao_em_andamento ?
                    "em sessão" : "em repouso",
                wifi.rede_principal[0] ? wifi.rede_principal : "não definida",
                static_cast<unsigned>(wifi.quantidade_redes_usuario));
    std::printf("  OTA: %s | SD: %s | CSV pendentes: %lu\r\n",
                servico_ota_nome_estado(ota.estado),
                sd.cartao_disponivel ? "pronto" : "indisponível",
                static_cast<unsigned long>(sd.arquivos_pendentes));
    std::printf("  Satélites: equipe %s, visitantes %s\r\n",
                saude.equipe_respondendo ? "respondendo" : "sem resposta",
                saude.visitantes_respondendo ? "respondendo" : "sem resposta");
}

void mostrar_menu_principal() {
    imprimir_titulo("UFSM-CS | ESP32-S3 MESTRE | MENU PRINCIPAL");
    std::printf("\r\n  MONITORAMENTO\r\n");
    imprimir_opcoes(OPCOES_PRINCIPAL_MONITORAMENTO, "    ");
    std::printf("\r\n  CONECTIVIDADE E MANUTENÇÃO\r\n");
    imprimir_opcoes(OPCOES_PRINCIPAL_CONECTIVIDADE, "    ");
    std::printf("\r\n  FERRAMENTAS E TERMINAL\r\n");
    imprimir_opcoes(OPCOES_PRINCIPAL_FERRAMENTAS, "    ");
    std::printf("\r\n    [0] Redesenhar este menu\r\n");
    imprimir_prompt("Digite uma opção e pressione Enter. Enter vazio redesenha o menu.");
}

void mostrar_menu_controle_veiculo() {
    const auto controle = servico_controle_remoto_obter_estado();
    imprimir_titulo("CONTROLE DO VEÍCULO | PEDAL");
    std::printf("  Pedal: %s | %u mV | %u,%u %%\r\n",
                controle.pedal_disponivel ? "lido" : "indisponível",
                controle.tensao_pedal_mv,
                controle.acelerador_lido_decimos_percentual / 10,
                controle.acelerador_lido_decimos_percentual % 10);
    std::printf("  DAC: %s | saída %s | comando %u mV\r\n",
                controle.dac_disponivel ? "presente" : "indisponível",
                controle.atuacao_habilitada ? "habilitada" : "bloqueada para validação",
                controle.tensao_dac_mv);
    std::printf("  Zona morta: %u,%u %% | saída mínima: %u mV | acelerador máximo: %u %%\r\n",
                controle.zona_morta_decimos_percentual / 10,
                controle.zona_morta_decimos_percentual % 10,
                controle.tensao_minima_saida_mv,
                controle.acelerador_maximo_decimos_percentual / 10);
    std::printf("  [1] Atualizar leituras\r\n  [2] Definir zona morta\r\n");
    std::printf("  [3] Definir tensão mínima de saída\r\n  [4] Definir acelerador máximo\r\n");
    std::printf("  [0] Voltar\r\n");
    imprimir_prompt("Configure com o pedal solto. As alterações duram até reiniciar.");
}

void mostrar_menu_intervalos() {
    imprimir_titulo("INTERVALOS DOS RELATÓRIOS");
    std::printf("Os valores abaixo alteram somente a exibição no terminal.\r\n");
    std::printf("Estado geral: %s\r\n\r\n",
                servico_relatorios_estao_pausados() ? "pausado" : "ativo");
    const size_t quantidade = static_cast<size_t>(GrupoRelatorio::Quantidade);
    for (size_t indice = 0; indice < quantidade; ++indice) {
        const auto grupo = static_cast<GrupoRelatorio>(indice);
        const uint32_t intervalo = servico_relatorios_obter_intervalo(grupo);
        const char* nome = servico_relatorios_nome_grupo(grupo);
        if (intervalo == 0) {
            std::printf("  [%u] %-25s desativado\r\n", static_cast<unsigned>(indice + 1),
                        nome);
        } else {
            std::printf("  [%u] %-25s %lu ms\r\n", static_cast<unsigned>(indice + 1),
                        nome, static_cast<unsigned long>(intervalo));
        }
    }
    std::printf("\r\n  [Enter] Voltar ao menu principal\r\n  comando > ");
    std::fflush(stdout);
}

void mostrar_opcoes_intervalo() {
    const size_t indice = static_cast<size_t>(grupo_em_edicao);
    imprimir_linha();
    std::printf("NOVO INTERVALO: %s\r\n\r\n",
                servico_relatorios_nome_grupo(
                    static_cast<GrupoRelatorio>(indice)));
    std::printf("  1 - 500 ms\r\n");
    std::printf("  2 - 1 segundo\r\n");
    std::printf("  3 - 2 segundos\r\n");
    std::printf("  4 - 5 segundos\r\n");
    std::printf("  5 - 10 segundos\r\n");
    std::printf("  6 - 30 segundos\r\n");
    std::printf("  7 - Desativar este relatório\r\n");
    std::printf("\r\nPressione somente Enter para voltar.\r\n> ");
    std::fflush(stdout);
}

bool interpretar_numero(const char* texto, unsigned long* numero) {
    if (texto == nullptr || numero == nullptr || texto[0] == '\0') return false;
    while (*texto == ' ' || *texto == '\t') texto++;
    if (*texto == '\0' || *texto == '+' || *texto == '-') return false;
    errno = 0;
    char* fim = nullptr;
    const unsigned long valor = std::strtoul(texto, &fim, 10);
    while (*fim == ' ' || *fim == '\t') fim++;
    if (errno != 0 || fim == texto || *fim != '\0') return false;
    *numero = valor;
    return true;
}

void alternar_pausa() {
    const bool pausados = servico_relatorios_alternar_pausa();
    std::printf("\r\n[OK] Relatórios periódicos %s.\r\n",
                pausados ? "pausados" : "retomados");
}

void tratar_menu_principal(unsigned long opcao) {
    switch (opcao) {
        case 0:
            mostrar_menu_principal();
            break;
        case 1:
            solicitar_resumo();
            mostrar_menu_principal();
            break;
        case 2:
            tela_atual = TelaTerminal::Operacao;
            terminal_operacao::abrir_ota();
            break;
        case 3:
            tela_atual = TelaTerminal::Intervalos;
            mostrar_menu_intervalos();
            break;
        case 4:
            alternar_pausa();
            mostrar_menu_principal();
            break;
        case 5:
            tela_atual = TelaTerminal::Wifi;
            terminal_wifi::abrir();
            break;
        case 6:
            tela_atual = TelaTerminal::Operacao;
            terminal_operacao::abrir_satelites();
            break;
        case 7:
            tela_atual = TelaTerminal::Sensores;
            terminal_sensores::abrir();
            break;
        case 8: {
            const EstadoRegistroTelemetria registro =
                servico_registro_telemetria_obter_estado();
            std::printf(
                "\r\nRegistro SD: %s | amostras=%lu | pendentes=%lu | enviados=%lu\r\n",
                registro.cartao_disponivel ? "ativo" : "indisponível",
                static_cast<unsigned long>(registro.amostras_gravadas),
                static_cast<unsigned long>(registro.arquivos_pendentes),
                static_cast<unsigned long>(registro.arquivos_enviados));
            const esp_err_t erro =
                servico_registro_telemetria_solicitar_sincronizacao();
            const ResumoWifi wifi_apos_solicitacao =
                servico_wifi_obter_resumo();
            if (erro == ESP_OK) {
                std::printf(
                    "[OK] Sincronização solicitada; o arquivo atual será fechado e enviado em segundo plano.\r\n");
            } else if (erro == ESP_ERR_INVALID_STATE &&
                       wifi_apos_solicitacao.sessao_em_andamento) {
                std::printf(
                    "[AVISO] Sincronização não iniciada: aguarde a conclusão da %s.\r\n",
                    servico_wifi_nome_finalidade_sessao(
                        wifi_apos_solicitacao.finalidade_sessao));
            } else {
                std::printf("[ERRO] Não foi possível iniciar a sincronização: %s.\r\n",
                            esp_err_to_name(erro));
            }
            mostrar_menu_principal();
            break;
        }
        case 10:
            tela_atual = TelaTerminal::Logs;
            mostrar_menu_logs();
            break;
        case 11:
            mostrar_estado_rapido();
            mostrar_menu_principal();
            break;
        case 12:
            tela_atual = TelaTerminal::ControleVeiculo;
            mostrar_menu_controle_veiculo();
            break;
        default:
            std::printf("\r\nOpção inválida. Escolha um número do menu.\r\n> ");
            std::fflush(stdout);
            break;
    }
}

void tratar_controle_veiculo(unsigned long opcao) {
    if (opcao == 0) {
        tela_atual = TelaTerminal::Principal;
        mostrar_menu_principal();
        return;
    }
    if (opcao == 1) {
        mostrar_menu_controle_veiculo();
        return;
    }
    if (opcao == 2) {
        tela_atual = TelaTerminal::ControleZonaMorta;
        imprimir_prompt("Zona morta em décimos de %: 0 a 300 (50 = 5%).");
        return;
    }
    if (opcao == 3) {
        tela_atual = TelaTerminal::ControleTensaoMinima;
        imprimir_prompt("Tensão mínima do DAC em mV: 0 a 2500.");
        return;
    }
    if (opcao == 4) {
        tela_atual = TelaTerminal::ControleAceleradorMaximo;
        imprimir_prompt("Acelerador máximo em %: 0 a 100.");
        return;
    }
    std::printf("\r\nOpção inválida.\r\n");
    mostrar_menu_controle_veiculo();
}

void tratar_valor_controle(unsigned long valor) {
    AjusteControleRemoto ajuste = AjusteControleRemoto::ZonaMortaPedal;
    uint32_t convertido = static_cast<uint32_t>(valor);
    if (tela_atual == TelaTerminal::ControleTensaoMinima)
        ajuste = AjusteControleRemoto::TensaoMinimaDac;
    else if (tela_atual == TelaTerminal::ControleAceleradorMaximo) {
        ajuste = AjusteControleRemoto::AceleradorMaximo;
        if (valor > 100) convertido = UINT32_MAX;
        else convertido = static_cast<uint32_t>(valor * 10);
    }
    const esp_err_t erro = servico_controle_remoto_definir(ajuste, convertido);
    std::printf("\r\n%s: %s.\r\n", erro == ESP_OK ? "[OK] Valor registrado" :
                "[ERRO] Ajuste recusado", esp_err_to_name(erro));
    tela_atual = TelaTerminal::ControleVeiculo;
    mostrar_menu_controle_veiculo();
}

void tratar_menu_intervalos(unsigned long opcao) {
    const size_t quantidade = static_cast<size_t>(GrupoRelatorio::Quantidade);
    if (opcao < 1 || opcao > quantidade) {
        std::printf("\r\nOpção inválida. Escolha um número do menu.\r\n");
        mostrar_menu_intervalos();
        return;
    }
    grupo_em_edicao = static_cast<GrupoRelatorio>(opcao - 1);
    tela_atual = TelaTerminal::EscolherIntervalo;
    mostrar_opcoes_intervalo();
}

void tratar_escolha_intervalo(unsigned long opcao) {
    constexpr uint32_t OPCOES_MS[] = {500, 1000, 2000, 5000, 10000, 30000, 0};
    if (opcao < 1 || opcao > (sizeof(OPCOES_MS) / sizeof(OPCOES_MS[0]))) {
        std::printf("\r\nOpção inválida. Escolha um número do menu.\r\n");
        mostrar_opcoes_intervalo();
        return;
    }

    const esp_err_t erro = servico_relatorios_definir_intervalo(
        grupo_em_edicao, OPCOES_MS[opcao - 1]);
    if (erro != ESP_OK) {
        std::printf("\r\n[ERRO] Não foi possível alterar o intervalo: %s\r\n",
                    esp_err_to_name(erro));
    } else {
        std::printf("\r\nIntervalo de '%s' atualizado.\r\n",
                    servico_relatorios_nome_grupo(grupo_em_edicao));
    }
    tela_atual = TelaTerminal::Intervalos;
    mostrar_menu_intervalos();
}

int ler_caractere_console() {
    if (caractere_pendente != EOF) {
        const int caractere = caractere_pendente;
        caractere_pendente = EOF;
        return caractere;
    }
    return std::getchar();
}

void consumir_segunda_quebra_de_linha() {
    if (!entrada_console_nao_bloqueante) return;

    // CRLF chega como dois bytes. O segundo precisa ser consumido antes que o
    // menu seja desenhado; caso contrário ele pareceria um novo Enter vazio.
    for (unsigned tentativa = 0; tentativa < 4; ++tentativa) {
        const int proximo = std::getchar();
        if (proximo == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        if (proximo != '\r' && proximo != '\n') {
            caractere_pendente = proximo;
        }
        return;
    }
}

void monitorar_sensor_ate_sair() {
    int64_t proxima_atualizacao_ms = 0;
    while (true) {
        const int64_t agora_ms = esp_timer_get_time() / 1000;
        if (agora_ms >= proxima_atualizacao_ms) {
            terminal_sensores::atualizar_monitor();
            proxima_atualizacao_ms =
                agora_ms + configuracao::INTERVALO_MONITOR_SENSORES_MS;
        }

        const int recebido = ler_caractere_console();
        if (recebido == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        const uint8_t caractere = static_cast<uint8_t>(recebido);
        if (caractere == '\r' || caractere == '\n' || caractere == 0x03) {
            if (caractere == '\r' || caractere == '\n') {
                consumir_segunda_quebra_de_linha();
            }
            return;
        }
        // A tela ao vivo não possui comandos: caracteres diferentes de Enter
        // são ignorados para não corromper o desenho durante a atualização.
    }
}

ResultadoLeituraLinha ler_linha_interativa(char* linha, size_t capacidade,
                                           EcoEntrada eco) {
    if (linha == nullptr || capacidade < 2) {
        return ResultadoLeituraLinha::LongaDemais;
    }

    size_t tamanho = 0;
    size_t caracteres_excedentes = 0;
    uint8_t caracteres_escape_restantes = 0;
    const bool silenciosa = eco == EcoEntrada::Silencioso;
    linha[0] = '\0';

    while (true) {
        const int recebido = ler_caractere_console();
        if (recebido == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        const uint8_t caractere = static_cast<uint8_t>(recebido);
        if (caractere == '\r' || caractere == '\n') {
            consumir_segunda_quebra_de_linha();
            std::printf("\r\n");
            linha[tamanho] = '\0';
            return caracteres_excedentes != 0
                       ? ResultadoLeituraLinha::LongaDemais
                       : ResultadoLeituraLinha::Concluida;
        }

        if (caracteres_escape_restantes != 0) {
            caracteres_escape_restantes--;
            continue;
        }
        if (caractere == 0x1B) {
            // Ignora sequências simples emitidas pelas setas do teclado.
            caracteres_escape_restantes = 2;
            continue;
        }
        if (caractere == 0x03) {
            // Ctrl+C tem o mesmo efeito de uma linha vazia: cancela a etapa.
            tamanho = 0;
            linha[0] = '\0';
            std::printf("^C\r\n");
            return ResultadoLeituraLinha::Concluida;
        }
        if (caractere == 0x15) {
            // Ctrl+U apaga toda a linha sem depender do programa de terminal.
            while (tamanho != 0) {
                if (!silenciosa) std::printf("\b \b");
                tamanho--;
            }
            caracteres_excedentes = 0;
            linha[0] = '\0';
            std::fflush(stdout);
            continue;
        }
        if (caractere == '\b' || caractere == 0x7F) {
            if (caracteres_excedentes != 0) {
                caracteres_excedentes--;
                continue;
            }
            if (tamanho != 0) {
                tamanho--;
                linha[tamanho] = '\0';
                if (!silenciosa) std::printf("\b \b");
                std::fflush(stdout);
            }
            continue;
        }
        if (caractere < 0x20 || caractere == 0x7F) continue;

        if (tamanho + 1 >= capacidade) {
            if (caracteres_excedentes == 0) {
                std::printf("\a");
                std::fflush(stdout);
            }
            caracteres_excedentes++;
            continue;
        }

        linha[tamanho++] = static_cast<char>(caractere);
        linha[tamanho] = '\0';
        if (!silenciosa) {
            std::putchar(eco == EcoEntrada::Mascarado ? '*' : static_cast<char>(caractere));
        }
        std::fflush(stdout);
    }
}

void voltar_uma_tela() {
    if (tela_atual == TelaTerminal::Wifi && terminal_wifi::voltar()) return;
    if (tela_atual == TelaTerminal::Sensores && terminal_sensores::voltar()) return;
    if (tela_atual == TelaTerminal::Operacao && terminal_operacao::voltar()) return;
    if (tela_atual == TelaTerminal::Principal) {
        mostrar_menu_principal();
    } else if (tela_atual == TelaTerminal::EscolherIntervalo) {
        tela_atual = TelaTerminal::Intervalos;
        mostrar_menu_intervalos();
    } else if (tela_atual == TelaTerminal::ControleZonaMorta ||
               tela_atual == TelaTerminal::ControleTensaoMinima ||
               tela_atual == TelaTerminal::ControleAceleradorMaximo) {
        tela_atual = TelaTerminal::ControleVeiculo;
        mostrar_menu_controle_veiculo();
    } else if (tela_atual == TelaTerminal::ControleVeiculo) {
        tela_atual = TelaTerminal::Principal;
        mostrar_menu_principal();
    } else if (tela_atual == TelaTerminal::Logs ||
               tela_atual == TelaTerminal::Operacao ||
               tela_atual == TelaTerminal::Sensores ||
               tela_atual == TelaTerminal::Intervalos ||
               tela_atual == TelaTerminal::Wifi) {
        tela_atual = TelaTerminal::Principal;
        mostrar_menu_principal();
    }
}

constexpr RotaTerminal ROTAS_TERMINAL[] = {
    {TelaTerminal::Principal, tratar_menu_principal},
    {TelaTerminal::Logs, tratar_menu_logs},
    {TelaTerminal::ControleVeiculo, tratar_controle_veiculo},
    {TelaTerminal::ControleZonaMorta, tratar_valor_controle},
    {TelaTerminal::ControleTensaoMinima, tratar_valor_controle},
    {TelaTerminal::ControleAceleradorMaximo, tratar_valor_controle},
    {TelaTerminal::Operacao, terminal_operacao::tratar_opcao},
    {TelaTerminal::Intervalos, tratar_menu_intervalos},
    {TelaTerminal::EscolherIntervalo, tratar_escolha_intervalo},
};

bool despachar_opcao(TelaTerminal tela, unsigned long opcao) {
    if (tela == TelaTerminal::Wifi) return terminal_wifi::tratar_opcao(opcao);
    if (tela == TelaTerminal::Sensores) return terminal_sensores::tratar_opcao(opcao);
    for (const RotaTerminal& rota : ROTAS_TERMINAL) {
        if (rota.tela == tela) {
            rota.tratar(opcao);
            return true;
        }
    }
    return false;
}

void tarefa_terminal(void*) {
    char linha[TAMANHO_MAXIMO_LINHA]{};
    // O console passa a entregar cada tecla imediatamente. A própria rotina de
    // leitura abaixo reconhece CR, LF e CRLF, independentemente do monitor usado.
    std::setvbuf(stdin, nullptr, _IONBF, 0);
    const int flags_entrada = fcntl(STDIN_FILENO, F_GETFL, 0);
    entrada_console_nao_bloqueante =
        flags_entrada >= 0 &&
        fcntl(STDIN_FILENO, F_SETFL, flags_entrada | O_NONBLOCK) == 0;
    if (!entrada_console_nao_bloqueante) {
        ESP_LOGW(ETIQUETA,
                 "Console não aceitou modo não bloqueante; CR e LF isolados continuam suportados");
    }
    mostrar_menu_principal();
    while (true) {
        if (tela_atual == TelaTerminal::Sensores && terminal_sensores::monitorando()) {
            monitorar_sensor_ate_sair();
            terminal_sensores::abrir();
            continue;
        }
        const bool em_menu_wifi = tela_atual == TelaTerminal::Wifi;
        const EcoEntrada eco =
            em_menu_wifi ? terminal_wifi::eco_entrada() : EcoEntrada::Normal;
        const ResultadoLeituraLinha leitura =
            ler_linha_interativa(linha, sizeof(linha), eco);
        if (leitura == ResultadoLeituraLinha::LongaDemais) {
            std::printf("\r\nEntrada longa demais. Operação cancelada.\r\n");
            voltar_uma_tela();
            continue;
        }
        if (linha[0] == '\0' && eco != EcoEntrada::Silencioso) {
            voltar_uma_tela();
            continue;
        }

        if (em_menu_wifi && terminal_wifi::tratar_texto(linha)) continue;

        unsigned long opcao = 0;
        if (!interpretar_numero(linha, &opcao)) {
            std::printf("\r\nEntrada inválida. Digite somente o número da opção.\r\n> ");
            std::fflush(stdout);
            continue;
        }

        if (!despachar_opcao(tela_atual, opcao)) {
            ESP_LOGE(ETIQUETA,
                     "Tela sem tratador numérico; retornando ao menu principal");
            tela_atual = TelaTerminal::Principal;
            mostrar_menu_principal();
        }
    }
}
}  // namespace

esp_err_t servico_terminal_obter_logs(EstadoLogsTerminal* estado) {
    if (estado == nullptr) return ESP_ERR_INVALID_ARG;
    const SemaphoreHandle_t mutex = mutex_logs.load(std::memory_order_acquire);
    if (mutex == nullptr) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(mutex, portMAX_DELAY);
    estado->nivel_global = nivel_global_logs;
    estado->apenas_avisos_projeto = apenas_avisos_projeto;
    estado->cores = terminal_cores.load(std::memory_order_relaxed);
    for (size_t i = 0; i < QUANTIDADE_FONTES_LOG_TERMINAL; ++i)
        estado->niveis_fontes[i] = fontes_log[i].nivel;
    xSemaphoreGive(mutex);
    return ESP_OK;
}

esp_err_t servico_terminal_definir_logs(AjusteLogsTerminal ajuste, uint8_t alvo,
                                      uint8_t valor) {
    const SemaphoreHandle_t mutex = mutex_logs.load(std::memory_order_acquire);
    if (mutex == nullptr) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(mutex, portMAX_DELAY);
    const esp_err_t erro = configurar_logs(ajuste, alvo, valor);
    xSemaphoreGive(mutex);
    return erro;
}

esp_err_t servico_terminal_iniciar() {
    if (iniciado) return ESP_OK;
    if (mutex_logs.load(std::memory_order_relaxed) == nullptr) {
        const auto mutex = xSemaphoreCreateMutexStatic(&memoria_mutex_logs);
        if (mutex == nullptr) return ESP_ERR_NO_MEM;
        if (apenas_avisos_projeto) aplicar_niveis_logs();
        mutex_logs.store(mutex, std::memory_order_release);
    }
    if (configuracao::criar_tarefa(tarefa_terminal, "terminal", nullptr, nullptr) != pdPASS) {
        ESP_LOGE(ETIQUETA, "Não foi possível iniciar o menu serial");
        return ESP_ERR_NO_MEM;
    }
    iniciado = true;
    return ESP_OK;
}
