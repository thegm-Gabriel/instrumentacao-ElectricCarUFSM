// Implementa os menus de atualização e consulta dos satélites.
#include "servicos/interface_local/terminal/interno.h"

#include <cstring>

#include "esp_timer.h"
#include "nucleo/configuracao/placa.h"
#include "servicos/comunicacao/comandos_uart.h"
#include "servicos/comunicacao/telemetria_uart.h"
#include "servicos/comunicacao/wifi_sessoes.h"
#include "servicos/manutencao/ota.h"

namespace terminal_operacao {
namespace {
using terminal_console::OpcaoMenu;
using terminal_console::imprimir_linha;
using terminal_console::imprimir_opcoes;
using terminal_console::imprimir_prompt;
using terminal_console::imprimir_titulo;

enum class TelaOperacao {
    Ota,
    OtaIntervalo,
    OtaIntervaloTelemetria,
    Satelites,
};

TelaOperacao tela_atual = TelaOperacao::Ota;

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
    {6, "Atualizar todos: satélites e depois mestre"},
    {7, "Cancelar atualizações pendentes"},
    {8, "Alterar intervalo das verificações"},
    {9, "Alterar intervalo da telemetria durante OTA"},
};

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
        std::printf("\r\n[OK] %s: firmware=%s.\r\n",
                    nome, versao.versao);
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
    tela_atual = TelaOperacao::Ota;
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
    tela_atual = TelaOperacao::Ota;
    mostrar_menu_ota();
}

void informar_resultado_ota(const char* acao, esp_err_t resultado) {
    const ResumoWifi wifi = servico_wifi_obter_resumo();
    if (resultado == ESP_OK) {
        std::printf("\r\n[OK] Solicitação recebida: %s.\r\n", acao);
    } else if (resultado == ESP_ERR_INVALID_STATE &&
               wifi.sessao_em_andamento) {
        std::printf(
            "\r\n[AVISO] Não foi possível %s: aguarde a conclusão da %s.\r\n",
            acao,
            servico_wifi_nome_finalidade_sessao(wifi.finalidade_sessao));
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
            informar_resultado_ota(
                "autorizar a atualização de todos os controladores",
                servico_ota_autorizar_atualizacao_todos());
            break;
        case 7:
            informar_resultado_ota("cancelar as atualizações pendentes",
                                   servico_ota_cancelar_atualizacao());
            break;
        case 8:
            tela_atual = TelaOperacao::OtaIntervalo;
            mostrar_intervalos_ota();
            return;
        case 9:
            tela_atual = TelaOperacao::OtaIntervaloTelemetria;
            mostrar_intervalos_telemetria_ota();
            return;
        default:
            std::printf("\r\nOpção inválida. Escolha um número do menu.\r\n");
            break;
    }
    mostrar_menu_ota();
}

}  // namespace

void abrir_ota() {
    tela_atual = TelaOperacao::Ota;
    mostrar_menu_ota();
}

void abrir_satelites() {
    tela_atual = TelaOperacao::Satelites;
    mostrar_menu_comandos_satelites();
}

bool voltar() {
    if (tela_atual == TelaOperacao::Ota || tela_atual == TelaOperacao::Satelites) {
        return false;
    }
    abrir_ota();
    return true;
}

void tratar_opcao(unsigned long opcao) {
    switch (tela_atual) {
        case TelaOperacao::Ota:
            tratar_menu_ota(opcao);
            break;
        case TelaOperacao::OtaIntervalo:
            tratar_intervalo_ota(opcao);
            break;
        case TelaOperacao::OtaIntervaloTelemetria:
            tratar_intervalo_telemetria_ota(opcao);
            break;
        case TelaOperacao::Satelites:
            tratar_menu_comandos_satelites(opcao);
            break;
    }
}
}  // namespace terminal_operacao
