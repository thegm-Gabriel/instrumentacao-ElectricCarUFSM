#include "servicos/servico_terminal.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "servicos/servico_comandos.h"
#include "servicos/servico_ota.h"
#include "servicos/servico_relatorios.h"
#include "servicos/servico_telemetria.h"
#include "servicos/servico_wifi.h"

namespace {
constexpr char ETIQUETA[] = "terminal";
constexpr size_t TAMANHO_MAXIMO_LINHA = 96;
bool iniciado = false;

enum class TelaTerminal : uint8_t {
    Principal,
    Ota,
    OtaIntervalo,
    OtaIntervaloTelemetria,
    Intervalos,
    EscolherIntervalo,
    ComandosSatelites,
    Wifi,
    WifiNovaSsid,
    WifiNovaSenha,
    WifiNovaConfirmarConexao,
    WifiNovaConfirmarSalvar,
    WifiEscolherConexao,
    WifiConfirmarConexao,
    WifiEscolherExclusao,
    WifiConfirmarExclusao,
    WifiEscolherAutomatico,
    WifiSenhaAutomatico,
    WifiSenhaPrincipal,
    WifiPrincipalSsid,
    WifiPrincipalSenha,
    WifiPrincipalConfirmar,
    WifiSenhaRestaurar,
    WifiRestaurarConfirmar,
    WifiPolitica,
    WifiPoliticaValor,
    WifiSenhaPolitica,
};

enum class CampoPoliticaWifi : uint8_t {
    Tentativas,
    TempoTentativa,
    IntervaloTentativas,
    TempoSessao,
    RestaurarPadroes,
};

struct OpcaoMenu {
    uint8_t numero;
    const char* descricao;
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
    {3, "Configurar intervalos dos relatórios"},
    {4, "Pausar ou retomar relatórios periódicos"},
};

constexpr OpcaoMenu OPCOES_PRINCIPAL_CONECTIVIDADE[] = {
    {2, "Atualização OTA"},
    {5, "Redes Wi-Fi"},
    {6, "Consultar satélites"},
};

constexpr OpcaoMenu OPCOES_SATELITES[] = {
    {1, "Ping do satélite da equipe"},
    {2, "Ping do satélite de visitantes"},
    {3, "Versão do satélite da equipe"},
    {4, "Versão do satélite de visitantes"},
};

constexpr OpcaoMenu OPCOES_OTA[] = {
    {1, "Atualizar o estado exibido"},
    {2, "Verificar nova versão agora"},
    {3, "Autorizar atualização do mestre"},
    {4, "Autorizar atualização do satélite da equipe"},
    {5, "Autorizar atualização do satélite de visitantes"},
    {6, "Cancelar atualizações pendentes"},
    {7, "Alterar intervalo das verificações"},
    {8, "Alterar intervalo da telemetria durante OTA"},
};

TelaTerminal tela_atual = TelaTerminal::Principal;
GrupoRelatorioTerminal grupo_em_edicao = GrupoRelatorioTerminal::Telemetria;
char wifi_ssid_pendente[33]{};
char wifi_senha_pendente[64]{};
char wifi_permissao_pendente[64]{};
size_t wifi_indice_pendente = 0;
bool wifi_automatico_pendente = false;
PoliticaConexaoWifi wifi_politica_pendente{};
CampoPoliticaWifi wifi_campo_pendente = CampoPoliticaWifi::Tentativas;
int caractere_pendente = EOF;
bool entrada_console_nao_bloqueante = false;

template <size_t Quantidade>
void imprimir_opcoes(const OpcaoMenu (&opcoes)[Quantidade],
                     const char* recuo = "  ") {
    for (const OpcaoMenu& opcao : opcoes) {
        std::printf("%s[%u] %s\r\n", recuo,
                    static_cast<unsigned>(opcao.numero), opcao.descricao);
    }
}

void imprimir_prompt(const char* orientacao, const char* rotulo = "comando") {
    if (orientacao != nullptr && orientacao[0] != '\0') {
        std::printf("\r\n  %s", orientacao);
    }
    std::printf("\r\n  %s > ", rotulo);
    std::fflush(stdout);
}

void imprimir_linha() {
    std::printf("\r\n============================================================\r\n");
}

void imprimir_titulo(const char* titulo) {
    imprimir_linha();
    std::printf("  %s\r\n", titulo);
    std::printf("============================================================\r\n");
}

void mostrar_menu_principal() {
    imprimir_titulo("UFSM-CS | ESP32-S3 MESTRE | MENU PRINCIPAL");
    std::printf("\r\n  MONITORAMENTO\r\n");
    imprimir_opcoes(OPCOES_PRINCIPAL_MONITORAMENTO, "    ");
    std::printf("\r\n  CONECTIVIDADE E MANUTENÇÃO\r\n");
    imprimir_opcoes(OPCOES_PRINCIPAL_CONECTIVIDADE, "    ");
    std::printf("\r\n    [0] Redesenhar este menu\r\n");
    imprimir_prompt("Digite uma opção e pressione Enter. Enter vazio redesenha o menu.");
}

void mostrar_menu_comandos_satelites() {
    imprimir_titulo("CONSULTAS AOS SATÉLITES");
    imprimir_opcoes(OPCOES_SATELITES);
    imprimir_prompt("[Enter] Voltar ao menu principal");
}

void mostrar_menu_ota() {
    const SituacaoOta ota = servico_ota_obter_situacao();
    imprimir_titulo("ATUALIZAÇÃO OTA");
    std::printf("Versão instalada : %s\r\n", ota.versao_atual[0] ? ota.versao_atual : "-");
    std::printf("Versão disponível: %s\r\n",
                ota.versao_disponivel[0] ? ota.versao_disponivel : "nenhuma");
    std::printf("Estado            : %s\r\n\r\n", servico_ota_nome_estado(ota.estado));
    auto mostrar_satelite = [](const char* nome, const SituacaoOtaSatelite& satelite) {
        std::printf("%-18s: %-25s  %s -> %s\r\n", nome,
                    servico_ota_nome_estado_satelite(satelite.estado),
                    satelite.versao_atual[0] ? satelite.versao_atual : "-",
                    satelite.versao_disponivel[0] ? satelite.versao_disponivel : "-");
        if (satelite.estado == EstadoOtaSatelite::Transferindo) {
            std::printf("                    %lu.%lu%% | %.2f MB/s | restante %lu s\r\n",
                        static_cast<unsigned long>(satelite.progresso.percentual_decimos / 10),
                        static_cast<unsigned long>(satelite.progresso.percentual_decimos % 10),
                        satelite.progresso.taxa_bytes_por_segundo / (1024.0 * 1024.0),
                        static_cast<unsigned long>(satelite.progresso.tempo_restante_segundos));
        }
    };
    mostrar_satelite("Satélite equipe", ota.equipe);
    mostrar_satelite("Satélite visitantes", ota.visitantes);
    if (ota.estado == EstadoServicoOta::Baixando && ota.alvo_ativo == AlvoOta::Mestre) {
        std::printf("Progresso mestre  : %lu.%lu%% | %.2f MB/s | restante %lu s\r\n",
                    static_cast<unsigned long>(ota.progresso.percentual_decimos / 10),
                    static_cast<unsigned long>(ota.progresso.percentual_decimos % 10),
                    ota.progresso.taxa_bytes_por_segundo / (1024.0 * 1024.0),
                    static_cast<unsigned long>(ota.progresso.tempo_restante_segundos));
    }
    std::printf("Verificação periódica: a cada %lu minuto(s)\r\n\r\n",
                static_cast<unsigned long>(ota.intervalo_verificacao_minutos));
    std::printf("Telemetria durante OTA: a cada %lu ms\r\n\r\n",
                static_cast<unsigned long>(
                    servico_telemetria_obter_intervalo_durante_ota()));
    imprimir_opcoes(OPCOES_OTA);
    imprimir_prompt("[Enter] Voltar ao menu principal");
}

void mostrar_intervalos_ota() {
    imprimir_linha();
    std::printf("INTERVALO DAS VERIFICAÇÕES OTA\r\n\r\n");
    std::printf("  1 - 5 minutos\r\n");
    std::printf("  2 - 15 minutos\r\n");
    std::printf("  3 - 30 minutos\r\n");
    std::printf("  4 - 1 hora\r\n");
    std::printf("  5 - 6 horas\r\n");
    std::printf("  6 - 24 horas\r\n");
    std::printf("\r\nPressione somente Enter para voltar.\r\n> ");
    std::fflush(stdout);
}

void mostrar_intervalos_telemetria_ota() {
    imprimir_linha();
    std::printf("TELEMETRIA DURANTE A OTA DOS SATÉLITES\r\n\r\n");
    std::printf("A telemetria permanece ativa, mas usa menos espaço na UART.\r\n\r\n");
    std::printf("  1 - 500 ms (mesmo intervalo normal)\r\n");
    std::printf("  2 - 1 segundo\r\n");
    std::printf("  3 - 2 segundos (recomendado)\r\n");
    std::printf("  4 - 5 segundos\r\n");
    std::printf("  5 - 10 segundos\r\n");
    std::printf("\r\nPressione somente Enter para voltar.\r\n> ");
    std::fflush(stdout);
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

void mostrar_redes_salvas(const ResumoWifi& wifi) {
    if (wifi.quantidade_redes_usuario == 0) {
        std::printf("  Nenhuma rede secundária salva.\r\n");
        return;
    }
    for (uint8_t indice = 0; indice < wifi.quantidade_redes_usuario; ++indice) {
        std::printf("  %u - %s\r\n", static_cast<unsigned>(indice + 1),
                    wifi.redes_usuario[indice]);
    }
}

void mostrar_menu_wifi() {
    const ResumoWifi wifi = servico_wifi_obter_resumo();
    imprimir_titulo("REDES WI-FI");
    std::printf("Estado             : %s\r\n", wifi.conectado ? "conectado" : "desconectado");
    std::printf("Rádio              : %s\r\n", wifi.radio_ativo ? "ativo" : "em repouso");
    std::printf("Rede ativa         : %s\r\n", wifi.rede_ativa[0] ? wifi.rede_ativa : "-");
    std::printf("Rede principal     : %s%s\r\n",
                wifi.rede_principal[0] ? wifi.rede_principal : "-",
                wifi.principal_alterada ? " (alterada)" : " (definida no firmware)");
    std::printf("Redes secundárias  : %u de %u\r\n",
                static_cast<unsigned>(wifi.quantidade_redes_usuario),
                static_cast<unsigned>(configuracao::MAXIMO_REDES_WIFI_USUARIO));
    std::printf("Conexão automática : %s\r\n\r\n",
                wifi.conectar_secundarias_automaticamente ? "habilitada" : "desabilitada");
    std::printf("Política de conexão: %u tentativa(s) por rede | %lu ms por tentativa\r\n",
                static_cast<unsigned>(wifi.politica.tentativas_por_rede),
                static_cast<unsigned long>(wifi.politica.tempo_por_tentativa_ms));
    std::printf("                     intervalo %lu ms | limite da sessão %lu s\r\n",
                static_cast<unsigned long>(
                    wifi.politica.intervalo_entre_tentativas_ms),
                static_cast<unsigned long>(
                    wifi.politica.tempo_limite_sessao_ms / 1000));
    if (wifi.sessao_em_andamento) {
        std::printf("Sessão de rede     : em andamento (%lu tentativa(s))\r\n",
                    static_cast<unsigned long>(
                        wifi.tentativas_ultima_sessao));
    }
    if (wifi.conectado) {
        std::printf("Sinal              : %d dBm (canal %u)\r\n",
                    wifi.rssi_dbm, static_cast<unsigned>(wifi.canal));
    }
    std::printf("Conexões concluídas: %lu | desconexões inesperadas: %lu\r\n",
                static_cast<unsigned long>(wifi.conexoes_bem_sucedidas),
                static_cast<unsigned long>(wifi.desconexoes_inesperadas));
    if (wifi.aguardando_autorizacao_secundaria) {
        std::printf("Atenção: a rede principal falhou e uma rede secundária aguarda autorização.\r\n");
    }
    std::printf("\r\n");
    std::printf("  [1] Atualizar o estado exibido\r\n");
    std::printf("  [2] Informar e testar uma nova rede\r\n");
    std::printf("  [3] Autorizar conexão a uma rede salva\r\n");
    std::printf("  [4] Excluir uma rede salva\r\n");
    std::printf("  [5] Alterar o comportamento automático\r\n");
    std::printf("  [6] Alterar a rede principal (protegido)\r\n");
    std::printf("  [7] Restaurar a rede do firmware (protegido)\r\n");
    std::printf("  [8] Configurar tentativas e tempos (protegido)\r\n");
    std::printf("\r\n  [Enter] Voltar ao menu principal\r\n  comando > ");
    std::fflush(stdout);
}

void solicitar_ssid_nova_rede() {
    std::memset(wifi_ssid_pendente, 0, sizeof(wifi_ssid_pendente));
    std::memset(wifi_senha_pendente, 0, sizeof(wifi_senha_pendente));
    imprimir_linha();
    std::printf("NOVA REDE WI-FI\r\n");
    std::printf("Digite o nome da rede (SSID) e pressione Enter para enviar.\r\n");
    std::printf("Enter vazio cancela.\r\n> ");
    std::fflush(stdout);
}

void solicitar_senha_rede(const char* titulo) {
    std::printf("\r\n%s\r\n", titulo);
    std::printf("Digite a senha (8 a 63 caracteres) e pressione Enter para enviar.\r\n");
    std::printf("A entrada será mascarada com '*'. Enter vazio cancela.\r\n> ");
    std::fflush(stdout);
}

void mostrar_confirmacao_nova_rede() {
    imprimir_linha();
    std::printf("CONFIRMAR TESTE DE CONEXÃO\r\n");
    std::printf("Rede: %s\r\n", wifi_ssid_pendente);
    std::printf("A senha não será exibida.\r\n\r\n");
    std::printf("  1 - Autorizar a tentativa de conexão\r\n");
    std::printf("  2 - Cancelar\r\n> ");
    std::fflush(stdout);
}

void mostrar_confirmacao_nova_principal() {
    imprimir_linha();
    std::printf("CONFIRMAR ALTERAÇÃO DA REDE PRINCIPAL\r\n");
    std::printf("Nova rede principal: %s\r\n", wifi_ssid_pendente);
    std::printf("A senha da rede não será exibida.\r\n\r\n");
    std::printf("  1 - Testar, salvar e usar como principal\r\n");
    std::printf("  2 - Cancelar\r\n> ");
    std::fflush(stdout);
}

void mostrar_confirmacao_restaurar_principal() {
    imprimir_linha();
    std::printf("CONFIRMAR RESTAURAÇÃO DA REDE PRINCIPAL\r\n");
    std::printf("O ESP tentará a rede definida no firmware.\r\n\r\n");
    std::printf("  1 - Confirmar\r\n");
    std::printf("  2 - Cancelar\r\n> ");
    std::fflush(stdout);
}

void mostrar_confirmacao_salvar_rede() {
    imprimir_linha();
    std::printf("REDE VALIDADA COM SUCESSO\r\n");
    std::printf("Deseja salvar a rede '%s' para uso futuro?\r\n\r\n",
                wifi_ssid_pendente);
    std::printf("  1 - Salvar rede\r\n");
    std::printf("  2 - Não salvar\r\n> ");
    std::fflush(stdout);
}

void mostrar_escolha_rede(const char* titulo) {
    const ResumoWifi wifi = servico_wifi_obter_resumo();
    imprimir_linha();
    std::printf("%s\r\n\r\n", titulo);
    mostrar_redes_salvas(wifi);
    std::printf("\r\nDigite o número da rede e pressione Enter. Enter vazio cancela.\r\n> ");
    std::fflush(stdout);
}

void mostrar_confirmacao_rede_salva(const char* acao) {
    const ResumoWifi wifi = servico_wifi_obter_resumo();
    imprimir_linha();
    if (wifi_indice_pendente >= wifi.quantidade_redes_usuario) {
        std::printf("A rede selecionada não existe mais.\r\n");
        tela_atual = TelaTerminal::Wifi;
        mostrar_menu_wifi();
        return;
    }
    std::printf("CONFIRMAR %s\r\n", acao);
    std::printf("Rede: %s\r\n\r\n", wifi.redes_usuario[wifi_indice_pendente]);
    std::printf("  1 - Confirmar\r\n");
    std::printf("  2 - Cancelar\r\n> ");
    std::fflush(stdout);
}

void mostrar_menu_automatico() {
    const ResumoWifi wifi = servico_wifi_obter_resumo();
    imprimir_linha();
    std::printf("CONEXÃO AUTOMÁTICA A REDES SECUNDÁRIAS\r\n");
    std::printf("Estado atual: %s\r\n\r\n",
                wifi.conectar_secundarias_automaticamente ? "habilitada" : "desabilitada");
    std::printf("  1 - Habilitar tentativas automáticas\r\n");
    std::printf("  2 - Exigir autorização antes de cada rede secundária\r\n");
    std::printf("\r\nPressione somente Enter para voltar.\r\n> ");
    std::fflush(stdout);
}

void mostrar_menu_politica_wifi() {
    const ResumoWifi wifi = servico_wifi_obter_resumo();
    imprimir_titulo("POLÍTICA DE CONEXÃO WI-FI");
    std::printf("  [1] Tentativas por rede       : %u (faixa %u a %u)\r\n",
                static_cast<unsigned>(wifi.politica.tentativas_por_rede),
                static_cast<unsigned>(configuracao::TENTATIVAS_WIFI_MINIMAS),
                static_cast<unsigned>(configuracao::TENTATIVAS_WIFI_MAXIMAS));
    std::printf("  [2] Tempo de cada tentativa   : %lu s\r\n",
                static_cast<unsigned long>(
                    wifi.politica.tempo_por_tentativa_ms / 1000));
    std::printf("  [3] Intervalo entre tentativas: %lu ms\r\n",
                static_cast<unsigned long>(
                    wifi.politica.intervalo_entre_tentativas_ms));
    std::printf("  [4] Limite total da sessão    : %lu s\r\n",
                static_cast<unsigned long>(
                    wifi.politica.tempo_limite_sessao_ms / 1000));
    std::printf("  [5] Restaurar valores padrão\r\n");
    std::printf("\r\nO limite da sessão inclui todas as redes tentadas.\r\n");
    std::printf("Pressione somente Enter para voltar.\r\n> ");
    std::fflush(stdout);
}

void solicitar_valor_politica_wifi() {
    imprimir_linha();
    switch (wifi_campo_pendente) {
        case CampoPoliticaWifi::Tentativas:
            std::printf("TENTATIVAS POR REDE\r\nDigite um valor de %u a %u.\r\n> ",
                        static_cast<unsigned>(
                            configuracao::TENTATIVAS_WIFI_MINIMAS),
                        static_cast<unsigned>(
                            configuracao::TENTATIVAS_WIFI_MAXIMAS));
            break;
        case CampoPoliticaWifi::TempoTentativa:
            std::printf("TEMPO POR TENTATIVA\r\nDigite o tempo em segundos, de %lu a %lu.\r\n> ",
                        static_cast<unsigned long>(
                            configuracao::TEMPO_TENTATIVA_WIFI_MINIMO_MS / 1000),
                        static_cast<unsigned long>(
                            configuracao::TEMPO_TENTATIVA_WIFI_MAXIMO_MS / 1000));
            break;
        case CampoPoliticaWifi::IntervaloTentativas:
            std::printf("INTERVALO ENTRE TENTATIVAS\r\nDigite o tempo em ms, de %lu a %lu.\r\n> ",
                        static_cast<unsigned long>(
                            configuracao::INTERVALO_TENTATIVAS_WIFI_MINIMO_MS),
                        static_cast<unsigned long>(
                            configuracao::INTERVALO_TENTATIVAS_WIFI_MAXIMO_MS));
            break;
        case CampoPoliticaWifi::TempoSessao:
            std::printf("LIMITE TOTAL DA SESSÃO\r\nDigite o tempo em segundos, de %lu a %lu.\r\n> ",
                        static_cast<unsigned long>(
                            configuracao::TEMPO_LIMITE_SESSAO_WIFI_MINIMO_MS /
                            1000),
                        static_cast<unsigned long>(
                            configuracao::TEMPO_LIMITE_SESSAO_WIFI_MAXIMO_MS /
                            1000));
            break;
        case CampoPoliticaWifi::RestaurarPadroes:
            break;
    }
    std::fflush(stdout);
}

void solicitar_senha_permissao(const char* operacao) {
    imprimir_linha();
    std::printf("OPERAÇÃO PROTEGIDA: %s\r\n", operacao);
    std::printf("Digite a senha de permissão e pressione Enter para enviar.\r\n");
    std::printf("A entrada será mascarada e não será registrada nos logs.\r\n");
    std::printf("Enter vazio cancela.\r\n> ");
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

void marcar_resumo_pendente() {
    servico_relatorios_solicitar_resumo();
}

void solicitar_resumo() {
    marcar_resumo_pendente();
    std::printf("\r\n[OK] Resumo solicitado; será exibido no próximo ciclo.\r\n");
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
            tela_atual = TelaTerminal::Ota;
            mostrar_menu_ota();
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
            mostrar_menu_wifi();
            break;
        case 6:
            tela_atual = TelaTerminal::ComandosSatelites;
            mostrar_menu_comandos_satelites();
            break;
        default:
            std::printf("\r\nOpção inválida. Escolha um número do menu.\r\n> ");
            std::fflush(stdout);
            break;
    }
}

void consultar_satelite(DestinoUart destino, codigo_comando_t comando) {
    uint8_t resposta[COMANDO_TAMANHO_MAXIMO_CARGA]{};
    uint16_t tamanho = 0;
    codigo_resposta_comando_t resultado = RESPOSTA_COMANDO_TIMEOUT;
    const int64_t inicio_us = esp_timer_get_time();
    const esp_err_t erro = servico_comandos_solicitar(
        destino, comando, nullptr, 0, resposta, sizeof(resposta), &tamanho,
        &resultado, pdMS_TO_TICKS(700));
    const char* nome = destino == DestinoUart::Equipe ? "equipe" : "visitantes";
    if (erro != ESP_OK) {
        std::printf("\r\n[ERRO] %s não respondeu: %s.\r\n", nome,
                    esp_err_to_name(erro));
        return;
    }
    if (resultado != RESPOSTA_COMANDO_OK) {
        std::printf("\r\n[ERRO] %s recusou a consulta (código %u).\r\n", nome,
                    static_cast<unsigned>(resultado));
        return;
    }
    if (comando == COMANDO_PING && tamanho == sizeof(resposta_comando_ping_t)) {
        resposta_comando_ping_t ping{};
        std::memcpy(&ping, resposta, sizeof(ping));
        const uint32_t rtt_ms = static_cast<uint32_t>(
            (esp_timer_get_time() - inicio_us) / 1000);
        std::printf("\r\n[OK] %s respondeu em %lu ms; ativo há %lu ms.\r\n",
                    nome, static_cast<unsigned long>(rtt_ms),
                    static_cast<unsigned long>(ping.tempo_ativo_ms));
    } else if (comando == COMANDO_OBTER_VERSAO &&
               tamanho == sizeof(resposta_comando_versao_t)) {
        resposta_comando_versao_t versao{};
        std::memcpy(&versao, resposta, sizeof(versao));
        versao.versao[sizeof(versao.versao) - 1] = '\0';
        std::printf("\r\n[OK] %s: firmware=%s capacidades=0x%08lX.\r\n",
                    nome, versao.versao,
                    static_cast<unsigned long>(versao.capacidades));
    } else {
        std::printf("\r\n[ERRO] Resposta de %s possui tamanho incompatível.\r\n",
                    nome);
    }
}

void tratar_menu_comandos_satelites(unsigned long opcao) {
    switch (opcao) {
        case 1: consultar_satelite(DestinoUart::Equipe, COMANDO_PING); break;
        case 2: consultar_satelite(DestinoUart::Visitantes, COMANDO_PING); break;
        case 3:
            consultar_satelite(DestinoUart::Equipe, COMANDO_OBTER_VERSAO);
            break;
        case 4:
            consultar_satelite(DestinoUart::Visitantes, COMANDO_OBTER_VERSAO);
            break;
        default:
            std::printf("\r\nOpção inválida. Escolha um número do menu.\r\n");
            break;
    }
    mostrar_menu_comandos_satelites();
}

void informar_resultado_ota(const char* acao, esp_err_t resultado);

void tratar_intervalo_ota(unsigned long opcao) {
    constexpr uint32_t OPCOES_MINUTOS[] = {5, 15, 30, 60, 360, 1440};
    if (opcao < 1 || opcao > (sizeof(OPCOES_MINUTOS) / sizeof(OPCOES_MINUTOS[0]))) {
        std::printf("\r\nOpção inválida. Escolha um número do menu.\r\n");
        mostrar_intervalos_ota();
        return;
    }
    const uint32_t minutos = OPCOES_MINUTOS[opcao - 1];
    informar_resultado_ota("alterar o intervalo das verificações",
                           servico_ota_definir_intervalo_verificacao(minutos));
    tela_atual = TelaTerminal::Ota;
    mostrar_menu_ota();
}

void tratar_intervalo_telemetria_ota(unsigned long opcao) {
    constexpr uint32_t OPCOES_MS[] = {500, 1000, 2000, 5000, 10000};
    if (opcao < 1 || opcao > (sizeof(OPCOES_MS) / sizeof(OPCOES_MS[0]))) {
        std::printf("\r\nOpção inválida. Escolha um número do menu.\r\n");
        mostrar_intervalos_telemetria_ota();
        return;
    }
    const esp_err_t erro = servico_telemetria_definir_intervalo_durante_ota(
        OPCOES_MS[opcao - 1]);
    if (erro == ESP_OK) {
        std::printf("\r\n[OK] Intervalo da telemetria durante OTA atualizado.\r\n");
    } else {
        std::printf("\r\n[ERRO] Não foi possível alterar o intervalo: %s.\r\n",
                    esp_err_to_name(erro));
    }
    tela_atual = TelaTerminal::Ota;
    mostrar_menu_ota();
}

void informar_resultado_ota(const char* acao, esp_err_t resultado) {
    if (resultado == ESP_OK) {
        std::printf("\r\n[OK] Solicitação recebida: %s.\r\n", acao);
    } else {
        std::printf("\r\n[ERRO] Não foi possível %s: %s. Consulte o estado da OTA.\r\n",
                    acao, esp_err_to_name(resultado));
    }
}

void tratar_menu_ota(unsigned long opcao) {
    switch (opcao) {
        case 1:
            break;
        case 2:
            informar_resultado_ota("verificar atualizações",
                                   servico_ota_solicitar_verificacao());
            break;
        case 3:
            informar_resultado_ota("autorizar a atualização",
                                   servico_ota_autorizar_atualizacao());
            break;
        case 4:
            informar_resultado_ota("autorizar a atualização do satélite da equipe",
                servico_ota_autorizar_atualizacao_satelite(AlvoOta::Equipe));
            break;
        case 5:
            informar_resultado_ota("autorizar a atualização do satélite de visitantes",
                servico_ota_autorizar_atualizacao_satelite(AlvoOta::Visitantes));
            break;
        case 6:
            informar_resultado_ota("cancelar as atualizações pendentes",
                                   servico_ota_cancelar_atualizacao());
            break;
        case 7:
            tela_atual = TelaTerminal::OtaIntervalo;
            mostrar_intervalos_ota();
            return;
        case 8:
            tela_atual = TelaTerminal::OtaIntervaloTelemetria;
            mostrar_intervalos_telemetria_ota();
            return;
        default:
            std::printf("\r\nOpção inválida. Escolha um número do menu.\r\n");
            break;
    }
    mostrar_menu_ota();
}

void tratar_menu_intervalos(unsigned long opcao) {
    const size_t quantidade = static_cast<size_t>(GrupoRelatorio::Quantidade);
    if (opcao < 1 || opcao > quantidade) {
        std::printf("\r\nOpção inválida. Escolha um número do menu.\r\n");
        mostrar_menu_intervalos();
        return;
    }
    grupo_em_edicao = static_cast<GrupoRelatorioTerminal>(opcao - 1);
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

    const auto grupo = static_cast<GrupoRelatorio>(grupo_em_edicao);
    const esp_err_t erro = servico_relatorios_definir_intervalo(
        grupo, OPCOES_MS[opcao - 1]);
    if (erro != ESP_OK) {
        std::printf("\r\n[ERRO] Não foi possível alterar o intervalo: %s\r\n",
                    esp_err_to_name(erro));
    } else {
        std::printf("\r\nIntervalo de '%s' atualizado.\r\n",
                    servico_relatorios_nome_grupo(grupo));
    }
    tela_atual = TelaTerminal::Intervalos;
    mostrar_menu_intervalos();
}

void limpar_dados_wifi_pendentes() {
    std::memset(wifi_ssid_pendente, 0, sizeof(wifi_ssid_pendente));
    std::memset(wifi_senha_pendente, 0, sizeof(wifi_senha_pendente));
    std::memset(wifi_permissao_pendente, 0, sizeof(wifi_permissao_pendente));
    wifi_indice_pendente = 0;
    wifi_automatico_pendente = false;
    wifi_politica_pendente = {};
    wifi_campo_pendente = CampoPoliticaWifi::Tentativas;
}

void voltar_menu_wifi() {
    limpar_dados_wifi_pendentes();
    tela_atual = TelaTerminal::Wifi;
    mostrar_menu_wifi();
}

void informar_resultado_wifi(const char* sucesso, const char* falha,
                             esp_err_t resultado) {
    if (resultado == ESP_OK) {
        std::printf("\r\n[OK] %s\r\n", sucesso);
    } else {
        std::printf("\r\n[ERRO] %s: %s.\r\n", falha, esp_err_to_name(resultado));
    }
}

void tratar_menu_wifi(unsigned long opcao) {
    switch (opcao) {
        case 1:
            mostrar_menu_wifi();
            return;
        case 2:
            tela_atual = TelaTerminal::WifiNovaSsid;
            solicitar_ssid_nova_rede();
            return;
        case 3:
            tela_atual = TelaTerminal::WifiEscolherConexao;
            mostrar_escolha_rede("AUTORIZAR CONEXÃO A UMA REDE SALVA");
            return;
        case 4:
            tela_atual = TelaTerminal::WifiEscolherExclusao;
            mostrar_escolha_rede("EXCLUIR UMA REDE SALVA");
            return;
        case 5:
            tela_atual = TelaTerminal::WifiEscolherAutomatico;
            mostrar_menu_automatico();
            return;
        case 6:
            tela_atual = TelaTerminal::WifiSenhaPrincipal;
            solicitar_senha_permissao("alterar a rede principal");
            return;
        case 7:
            tela_atual = TelaTerminal::WifiSenhaRestaurar;
            solicitar_senha_permissao("restaurar a rede principal do firmware");
            return;
        case 8:
            tela_atual = TelaTerminal::WifiPolitica;
            mostrar_menu_politica_wifi();
            return;
        default:
            std::printf("\r\nOpção inválida. Escolha um número do menu.\r\n");
            mostrar_menu_wifi();
            return;
    }
}

void tratar_menu_politica_wifi(unsigned long opcao) {
    if (opcao < 1 || opcao > 5) {
        std::printf("\r\nOpção inválida. Escolha um número do menu.\r\n");
        mostrar_menu_politica_wifi();
        return;
    }
    wifi_politica_pendente = servico_wifi_obter_resumo().politica;
    if (opcao == 5) {
        wifi_politica_pendente = {};
        wifi_campo_pendente = CampoPoliticaWifi::RestaurarPadroes;
        tela_atual = TelaTerminal::WifiSenhaPolitica;
        solicitar_senha_permissao("restaurar a política Wi-Fi padrão");
        return;
    }
    wifi_campo_pendente =
        static_cast<CampoPoliticaWifi>(opcao - 1);
    tela_atual = TelaTerminal::WifiPoliticaValor;
    solicitar_valor_politica_wifi();
}

void tratar_valor_politica_wifi(unsigned long valor) {
    switch (wifi_campo_pendente) {
        case CampoPoliticaWifi::Tentativas:
            if (valor < configuracao::TENTATIVAS_WIFI_MINIMAS ||
                valor > configuracao::TENTATIVAS_WIFI_MAXIMAS) {
                std::printf("\r\nQuantidade de tentativas fora da faixa permitida.\r\n");
                solicitar_valor_politica_wifi();
                return;
            }
            wifi_politica_pendente.tentativas_por_rede =
                static_cast<uint8_t>(valor);
            break;
        case CampoPoliticaWifi::TempoTentativa:
            if (valor <
                    configuracao::TEMPO_TENTATIVA_WIFI_MINIMO_MS / 1000 ||
                valor >
                    configuracao::TEMPO_TENTATIVA_WIFI_MAXIMO_MS / 1000) {
                std::printf("\r\nTempo por tentativa fora da faixa permitida.\r\n");
                solicitar_valor_politica_wifi();
                return;
            }
            wifi_politica_pendente.tempo_por_tentativa_ms =
                static_cast<uint32_t>(valor * 1000UL);
            break;
        case CampoPoliticaWifi::IntervaloTentativas:
            if (valor <
                    configuracao::INTERVALO_TENTATIVAS_WIFI_MINIMO_MS ||
                valor >
                    configuracao::INTERVALO_TENTATIVAS_WIFI_MAXIMO_MS) {
                std::printf("\r\nIntervalo entre tentativas fora da faixa permitida.\r\n");
                solicitar_valor_politica_wifi();
                return;
            }
            wifi_politica_pendente.intervalo_entre_tentativas_ms =
                static_cast<uint32_t>(valor);
            break;
        case CampoPoliticaWifi::TempoSessao:
            if (valor <
                    configuracao::TEMPO_LIMITE_SESSAO_WIFI_MINIMO_MS / 1000 ||
                valor >
                    configuracao::TEMPO_LIMITE_SESSAO_WIFI_MAXIMO_MS / 1000) {
                std::printf("\r\nLimite da sessão fora da faixa permitida.\r\n");
                solicitar_valor_politica_wifi();
                return;
            }
            wifi_politica_pendente.tempo_limite_sessao_ms =
                static_cast<uint32_t>(valor * 1000UL);
            break;
        case CampoPoliticaWifi::RestaurarPadroes:
            break;
    }

    if (!servico_wifi_politica_valida(wifi_politica_pendente)) {
        std::printf(
            "\r\nValor inválido. Confira a faixa indicada; o limite da sessão "
            "também deve ser igual ou maior que o tempo de uma tentativa.\r\n");
        solicitar_valor_politica_wifi();
        return;
    }
    tela_atual = TelaTerminal::WifiSenhaPolitica;
    solicitar_senha_permissao("alterar a política de conexão Wi-Fi");
}

void tratar_confirmacao_nova_rede(unsigned long opcao) {
    if (opcao == 2) {
        voltar_menu_wifi();
        return;
    }
    if (opcao != 1) {
        std::printf("\r\nOpção inválida.\r\n");
        mostrar_confirmacao_nova_rede();
        return;
    }

    std::printf("\r\nTentando conectar à rede '%s'...\r\n", wifi_ssid_pendente);
    std::fflush(stdout);
    const esp_err_t erro = servico_wifi_conectar_rede_informada(
        wifi_ssid_pendente, wifi_senha_pendente);
    if (erro != ESP_OK) {
        informar_resultado_wifi("", "Não foi possível conectar", erro);
        voltar_menu_wifi();
        return;
    }
    tela_atual = TelaTerminal::WifiNovaConfirmarSalvar;
    mostrar_confirmacao_salvar_rede();
}

void tratar_confirmacao_salvar_rede(unsigned long opcao) {
    if (opcao == 1) {
        const esp_err_t erro = servico_wifi_salvar_rede_usuario(
            wifi_ssid_pendente, wifi_senha_pendente);
        informar_resultado_wifi("Rede salva com sucesso.",
                               "A rede foi conectada, mas não pôde ser salva", erro);
        voltar_menu_wifi();
        return;
    }
    if (opcao == 2) {
        std::printf("\r\nA rede não foi salva. O rádio Wi-Fi voltou ao repouso.\r\n");
        voltar_menu_wifi();
        return;
    }
    std::printf("\r\nOpção inválida.\r\n");
    mostrar_confirmacao_salvar_rede();
}

void tratar_escolha_rede_salva(unsigned long opcao, bool excluir) {
    const ResumoWifi wifi = servico_wifi_obter_resumo();
    if (opcao < 1 || opcao > wifi.quantidade_redes_usuario) {
        std::printf("\r\nRede inválida. Escolha um número da lista.\r\n");
        mostrar_escolha_rede(excluir ? "EXCLUIR UMA REDE SALVA" :
                                     "AUTORIZAR CONEXÃO A UMA REDE SALVA");
        return;
    }
    wifi_indice_pendente = static_cast<size_t>(opcao - 1);
    tela_atual = excluir ? TelaTerminal::WifiConfirmarExclusao
                         : TelaTerminal::WifiConfirmarConexao;
    mostrar_confirmacao_rede_salva(excluir ? "EXCLUSÃO" : "CONEXÃO");
}

void tratar_confirmacao_rede_salva(unsigned long opcao, bool excluir) {
    if (opcao == 2) {
        voltar_menu_wifi();
        return;
    }
    if (opcao != 1) {
        std::printf("\r\nOpção inválida.\r\n");
        mostrar_confirmacao_rede_salva(excluir ? "EXCLUSÃO" : "CONEXÃO");
        return;
    }
    const esp_err_t erro = excluir
        ? servico_wifi_excluir_rede_usuario(wifi_indice_pendente)
        : servico_wifi_conectar_rede_salva(wifi_indice_pendente);
    informar_resultado_wifi(excluir ? "Rede excluída com sucesso."
                                    : "Rede validada e autorizada para a próxima operação; o rádio voltou ao repouso.",
                           excluir ? "Não foi possível excluir a rede"
                                   : "Não foi possível conectar à rede",
                           erro);
    voltar_menu_wifi();
}

void tratar_escolha_automatico(unsigned long opcao) {
    if (opcao != 1 && opcao != 2) {
        std::printf("\r\nOpção inválida.\r\n");
        mostrar_menu_automatico();
        return;
    }
    wifi_automatico_pendente = opcao == 1;
    tela_atual = TelaTerminal::WifiSenhaAutomatico;
    solicitar_senha_permissao("alterar a autorização automática");
}

void tratar_confirmacao_principal(unsigned long opcao) {
    if (opcao == 2) {
        voltar_menu_wifi();
        return;
    }
    if (opcao != 1) {
        std::printf("\r\nOpção inválida.\r\n");
        mostrar_confirmacao_nova_principal();
        return;
    }
    const esp_err_t erro = servico_wifi_alterar_rede_principal(
        wifi_ssid_pendente, wifi_senha_pendente, wifi_permissao_pendente);
    informar_resultado_wifi("Rede principal alterada e salva com sucesso.",
                           "Não foi possível alterar a rede principal", erro);
    voltar_menu_wifi();
}

void tratar_confirmacao_restaurar(unsigned long opcao) {
    if (opcao == 2) {
        voltar_menu_wifi();
        return;
    }
    if (opcao != 1) {
        std::printf("\r\nOpção inválida.\r\n");
        mostrar_confirmacao_restaurar_principal();
        return;
    }
    const esp_err_t erro = servico_wifi_restaurar_rede_principal(
        wifi_permissao_pendente);
    informar_resultado_wifi("Rede principal do firmware restaurada com sucesso.",
                           "Não foi possível restaurar a rede principal", erro);
    voltar_menu_wifi();
}

bool tela_recebe_texto_wifi() {
    return tela_atual == TelaTerminal::WifiNovaSsid ||
           tela_atual == TelaTerminal::WifiNovaSenha ||
           tela_atual == TelaTerminal::WifiSenhaAutomatico ||
           tela_atual == TelaTerminal::WifiSenhaPrincipal ||
           tela_atual == TelaTerminal::WifiPrincipalSsid ||
           tela_atual == TelaTerminal::WifiPrincipalSenha ||
           tela_atual == TelaTerminal::WifiSenhaRestaurar ||
           tela_atual == TelaTerminal::WifiSenhaPolitica;
}

bool tela_oculta_entrada() {
    return tela_atual == TelaTerminal::WifiNovaSenha ||
           tela_atual == TelaTerminal::WifiSenhaAutomatico ||
           tela_atual == TelaTerminal::WifiSenhaPrincipal ||
           tela_atual == TelaTerminal::WifiPrincipalSenha ||
           tela_atual == TelaTerminal::WifiSenhaRestaurar ||
           tela_atual == TelaTerminal::WifiSenhaPolitica;
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

ResultadoLeituraLinha ler_linha_interativa(char* linha, size_t capacidade,
                                            bool ocultar) {
    if (linha == nullptr || capacidade < 2) {
        return ResultadoLeituraLinha::LongaDemais;
    }

    size_t tamanho = 0;
    size_t caracteres_excedentes = 0;
    uint8_t caracteres_escape_restantes = 0;
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
                std::printf("\b \b");
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
                std::printf("\b \b");
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
        std::putchar(ocultar ? '*' : static_cast<char>(caractere));
        std::fflush(stdout);
    }
}

void tratar_texto_wifi(const char* texto) {
    const size_t tamanho = std::strlen(texto);
    if (tela_atual == TelaTerminal::WifiNovaSsid ||
        tela_atual == TelaTerminal::WifiPrincipalSsid) {
        if (tamanho < 1 || tamanho > 32) {
            std::printf("\r\nO SSID deve ter entre 1 e 32 caracteres.\r\n> ");
            std::fflush(stdout);
            return;
        }
        std::strncpy(wifi_ssid_pendente, texto, sizeof(wifi_ssid_pendente) - 1);
        if (tela_atual == TelaTerminal::WifiNovaSsid) {
            tela_atual = TelaTerminal::WifiNovaSenha;
            solicitar_senha_rede("SENHA DA NOVA REDE");
        } else {
            tela_atual = TelaTerminal::WifiPrincipalSenha;
            solicitar_senha_rede("SENHA DA NOVA REDE PRINCIPAL");
        }
        return;
    }

    if (tela_atual == TelaTerminal::WifiNovaSenha ||
        tela_atual == TelaTerminal::WifiPrincipalSenha) {
        if (tamanho < 8 || tamanho > 63) {
            std::printf("\r\nA senha deve ter entre 8 e 63 caracteres.\r\n> ");
            std::fflush(stdout);
            return;
        }
        std::strncpy(wifi_senha_pendente, texto, sizeof(wifi_senha_pendente) - 1);
        if (tela_atual == TelaTerminal::WifiNovaSenha) {
            tela_atual = TelaTerminal::WifiNovaConfirmarConexao;
            mostrar_confirmacao_nova_rede();
        } else {
            tela_atual = TelaTerminal::WifiPrincipalConfirmar;
            mostrar_confirmacao_nova_principal();
        }
        return;
    }

    if (!servico_wifi_validar_permissao(texto)) {
        ESP_LOGW(ETIQUETA, "Operação Wi-Fi protegida recusada: senha de permissão incorreta");
        std::printf("\r\nSenha de permissão incorreta. Operação cancelada.\r\n");
        voltar_menu_wifi();
        return;
    }

    std::strncpy(wifi_permissao_pendente, texto,
                 sizeof(wifi_permissao_pendente) - 1);
    if (tela_atual == TelaTerminal::WifiSenhaAutomatico) {
        const esp_err_t erro = servico_wifi_definir_conexao_automatica(
            wifi_automatico_pendente, wifi_permissao_pendente);
        informar_resultado_wifi(
            wifi_automatico_pendente
                ? "Conexão automática às redes secundárias habilitada."
                : "As redes secundárias voltarão a exigir autorização.",
            "Não foi possível salvar a preferência", erro);
        voltar_menu_wifi();
    } else if (tela_atual == TelaTerminal::WifiSenhaPolitica) {
        const esp_err_t erro = servico_wifi_definir_politica(
            wifi_politica_pendente, wifi_permissao_pendente);
        informar_resultado_wifi(
            "Política de conexão Wi-Fi salva com sucesso.",
            "Não foi possível salvar a política de conexão", erro);
        voltar_menu_wifi();
    } else if (tela_atual == TelaTerminal::WifiSenhaPrincipal) {
        tela_atual = TelaTerminal::WifiPrincipalSsid;
        std::memset(wifi_ssid_pendente, 0, sizeof(wifi_ssid_pendente));
        std::memset(wifi_senha_pendente, 0, sizeof(wifi_senha_pendente));
        imprimir_linha();
        std::printf("NOVA REDE PRINCIPAL\r\n");
        std::printf("Digite o SSID e pressione Enter para enviar. Enter vazio cancela.\r\n> ");
        std::fflush(stdout);
    } else {
        tela_atual = TelaTerminal::WifiRestaurarConfirmar;
        mostrar_confirmacao_restaurar_principal();
    }
}

void voltar_uma_tela() {
    if (tela_atual == TelaTerminal::Principal) {
        mostrar_menu_principal();
    } else if (tela_atual == TelaTerminal::EscolherIntervalo) {
        tela_atual = TelaTerminal::Intervalos;
        mostrar_menu_intervalos();
    } else if (tela_atual == TelaTerminal::OtaIntervalo ||
               tela_atual == TelaTerminal::OtaIntervaloTelemetria) {
        tela_atual = TelaTerminal::Ota;
        mostrar_menu_ota();
    } else if (tela_atual == TelaTerminal::Ota ||
               tela_atual == TelaTerminal::Intervalos ||
               tela_atual == TelaTerminal::ComandosSatelites ||
               tela_atual == TelaTerminal::Wifi) {
        tela_atual = TelaTerminal::Principal;
        mostrar_menu_principal();
    } else if (tela_atual == TelaTerminal::WifiPoliticaValor ||
               tela_atual == TelaTerminal::WifiSenhaPolitica) {
        tela_atual = TelaTerminal::WifiPolitica;
        mostrar_menu_politica_wifi();
    } else if (tela_atual == TelaTerminal::WifiPolitica) {
        voltar_menu_wifi();
    } else {
        voltar_menu_wifi();
    }
}

void tratar_escolha_conexao(unsigned long opcao) {
    tratar_escolha_rede_salva(opcao, false);
}

void tratar_confirmacao_conexao(unsigned long opcao) {
    tratar_confirmacao_rede_salva(opcao, false);
}

void tratar_escolha_exclusao(unsigned long opcao) {
    tratar_escolha_rede_salva(opcao, true);
}

void tratar_confirmacao_exclusao(unsigned long opcao) {
    tratar_confirmacao_rede_salva(opcao, true);
}

constexpr RotaTerminal ROTAS_TERMINAL[] = {
    {TelaTerminal::Principal, tratar_menu_principal},
    {TelaTerminal::Ota, tratar_menu_ota},
    {TelaTerminal::OtaIntervalo, tratar_intervalo_ota},
    {TelaTerminal::OtaIntervaloTelemetria,
     tratar_intervalo_telemetria_ota},
    {TelaTerminal::Intervalos, tratar_menu_intervalos},
    {TelaTerminal::EscolherIntervalo, tratar_escolha_intervalo},
    {TelaTerminal::ComandosSatelites, tratar_menu_comandos_satelites},
    {TelaTerminal::Wifi, tratar_menu_wifi},
    {TelaTerminal::WifiPolitica, tratar_menu_politica_wifi},
    {TelaTerminal::WifiPoliticaValor, tratar_valor_politica_wifi},
    {TelaTerminal::WifiNovaConfirmarConexao, tratar_confirmacao_nova_rede},
    {TelaTerminal::WifiNovaConfirmarSalvar, tratar_confirmacao_salvar_rede},
    {TelaTerminal::WifiEscolherConexao, tratar_escolha_conexao},
    {TelaTerminal::WifiConfirmarConexao, tratar_confirmacao_conexao},
    {TelaTerminal::WifiEscolherExclusao, tratar_escolha_exclusao},
    {TelaTerminal::WifiConfirmarExclusao, tratar_confirmacao_exclusao},
    {TelaTerminal::WifiEscolherAutomatico, tratar_escolha_automatico},
    {TelaTerminal::WifiPrincipalConfirmar, tratar_confirmacao_principal},
    {TelaTerminal::WifiRestaurarConfirmar, tratar_confirmacao_restaurar},
};

bool despachar_opcao(TelaTerminal tela, unsigned long opcao) {
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
        const ResultadoLeituraLinha leitura = ler_linha_interativa(
            linha, sizeof(linha), tela_oculta_entrada());
        if (leitura == ResultadoLeituraLinha::LongaDemais) {
            std::printf("\r\nEntrada longa demais. Operação cancelada.\r\n");
            voltar_uma_tela();
            continue;
        }
        if (linha[0] == '\0') {
            voltar_uma_tela();
            continue;
        }

        if (tela_recebe_texto_wifi()) {
            tratar_texto_wifi(linha);
            continue;
        }

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

esp_err_t servico_terminal_iniciar() {
    if (iniciado) return ESP_OK;
    if (xTaskCreate(tarefa_terminal, "terminal", 4096, nullptr, 3, nullptr) != pdPASS) {
        ESP_LOGE(ETIQUETA, "Não foi possível iniciar o menu serial");
        return ESP_ERR_NO_MEM;
    }
    iniciado = true;
    return ESP_OK;
}

void servico_terminal_solicitar_resumo() {
    servico_relatorios_solicitar_resumo();
    ESP_LOGI(ETIQUETA, "Resumo solicitado pelo canal remoto da equipe");
}

bool servico_terminal_deve_exibir(GrupoRelatorioTerminal grupo) {
    return servico_relatorios_deve_exibir(grupo);
}
