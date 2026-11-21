#include "servicos/interface_local/terminal/interno.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "nucleo/configuracao/wifi.h"
#include "servicos/comunicacao/wifi_sessoes.h"

namespace terminal_wifi {
namespace {
using terminal_console::imprimir_linha;
using terminal_console::imprimir_titulo;
constexpr char ETIQUETA[] = "terminal";

enum class TelaWifi : uint8_t {
    Principal,
    NovaSsid,
    NovaSenha,
    NovaConfirmarConexao,
    NovaConfirmarSalvar,
    EscolherConexao,
    ConfirmarConexao,
    EscolherExclusao,
    ConfirmarExclusao,
    EscolherAutomatico,
    SenhaAutomatico,
    SenhaPrincipal,
    PrincipalOrigem,
    PrincipalEscolherSalva,
    PrincipalConfirmarSalva,
    PrincipalSsid,
    PrincipalSenha,
    PrincipalConfirmar,
    SenhaRestaurar,
    RestaurarConfirmar,
    Politica,
    PoliticaValor,
    SenhaPolitica,
};

enum class CampoPoliticaWifi : uint8_t {
    Tentativas,
    TempoTentativa,
    IntervaloTentativas,
    TempoSessao,
    RestaurarPadroes,
};

struct ContextoWifiTerminal {
    char ssid[33]{};
    char senha[64]{};
    char permissao[64]{};
    size_t indice_rede = 0;
    bool automatico = false;
    PoliticaConexaoWifi politica{};
    CampoPoliticaWifi campo_politica = CampoPoliticaWifi::Tentativas;
};

ContextoWifiTerminal contexto_wifi{};

TelaWifi tela_atual = TelaWifi::Principal;

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
        std::printf("Sessão de rede     : %s (%lu tentativa(s))\r\n",
                    servico_wifi_nome_finalidade_sessao(
                        wifi.finalidade_sessao),
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
    std::printf("  [6] Definir rede principal: nova ou salva (protegido)\r\n");
    std::printf("  [7] Restaurar a rede do firmware (protegido)\r\n");
    std::printf("  [8] Configurar tentativas e tempos (protegido)\r\n");
    std::printf("\r\n  [Enter] Voltar ao menu principal\r\n  comando > ");
    std::fflush(stdout);
}

void solicitar_ssid_nova_rede() {
    std::memset(contexto_wifi.ssid, 0, sizeof(contexto_wifi.ssid));
    std::memset(contexto_wifi.senha, 0, sizeof(contexto_wifi.senha));
    imprimir_linha();
    std::printf("NOVA REDE WI-FI\r\n");
    std::printf("Digite o nome da rede (SSID) e pressione Enter para enviar.\r\n");
    std::printf("Enter vazio cancela.\r\n> ");
    std::fflush(stdout);
}

void solicitar_senha_rede(const char* titulo) {
    std::printf("\r\n%s\r\n", titulo);
    std::printf("Senha vazia para rede aberta; caso contrário, 8 a 63 caracteres.\r\n");
    std::printf("A entrada não aparece no terminal.\r\n> ");
    std::fflush(stdout);
}

void mostrar_confirmacao_nova_rede() {
    imprimir_linha();
    std::printf("CONFIRMAR TESTE DE CONEXÃO\r\n");
    std::printf("Rede: %s\r\n", contexto_wifi.ssid);
    std::printf("A senha não será exibida.\r\n\r\n");
    std::printf("  1 - Autorizar a tentativa de conexão\r\n");
    std::printf("  2 - Cancelar\r\n> ");
    std::fflush(stdout);
}

void mostrar_confirmacao_nova_principal() {
    imprimir_linha();
    std::printf("CONFIRMAR ALTERAÇÃO DA REDE PRINCIPAL\r\n");
    std::printf("Nova rede principal: %s\r\n", contexto_wifi.ssid);
    std::printf("A senha da rede não será exibida.\r\n\r\n");
    std::printf("  1 - Testar, salvar e usar como principal\r\n");
    std::printf("  2 - Cancelar\r\n> ");
    std::fflush(stdout);
}

void mostrar_origem_rede_principal() {
    imprimir_linha();
    std::printf("DEFINIR REDE PRINCIPAL\r\n\r\n");
    std::printf("  1 - Informar uma nova rede\r\n");
    std::printf("  2 - Escolher uma rede já salva\r\n");
    std::printf("\r\nEnter vazio cancela.\r\n> ");
    std::fflush(stdout);
}

void mostrar_confirmacao_principal_salva() {
    const ResumoWifi wifi = servico_wifi_obter_resumo();
    if (contexto_wifi.indice_rede >= wifi.quantidade_redes_usuario) {
        std::printf("Rede salva indisponível.\r\n");
        tela_atual = TelaWifi::Principal;
        mostrar_menu_wifi();
        return;
    }
    imprimir_linha();
    std::printf("DEFINIR REDE SALVA COMO PRINCIPAL\r\n");
    std::printf("Rede: %s\r\n", wifi.redes_usuario[contexto_wifi.indice_rede]);
    std::printf("A conexão será testada antes de gravar.\r\n\r\n");
    std::printf("  1 - Confirmar\r\n  2 - Cancelar\r\n> ");
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
                contexto_wifi.ssid);
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
    if (contexto_wifi.indice_rede >= wifi.quantidade_redes_usuario) {
        std::printf("A rede selecionada não existe mais.\r\n");
        tela_atual = TelaWifi::Principal;
        mostrar_menu_wifi();
        return;
    }
    std::printf("CONFIRMAR %s\r\n", acao);
    std::printf("Rede: %s\r\n\r\n",
                wifi.redes_usuario[contexto_wifi.indice_rede]);
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
    switch (contexto_wifi.campo_politica) {
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

void limpar_dados_wifi_pendentes() {
    contexto_wifi = {};
}

void voltar_menu_wifi() {
    limpar_dados_wifi_pendentes();
    tela_atual = TelaWifi::Principal;
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
            tela_atual = TelaWifi::NovaSsid;
            solicitar_ssid_nova_rede();
            return;
        case 3:
            tela_atual = TelaWifi::EscolherConexao;
            mostrar_escolha_rede("AUTORIZAR CONEXÃO A UMA REDE SALVA");
            return;
        case 4:
            tela_atual = TelaWifi::EscolherExclusao;
            mostrar_escolha_rede("EXCLUIR UMA REDE SALVA");
            return;
        case 5:
            tela_atual = TelaWifi::EscolherAutomatico;
            mostrar_menu_automatico();
            return;
        case 6:
            tela_atual = TelaWifi::SenhaPrincipal;
            solicitar_senha_permissao("alterar a rede principal");
            return;
        case 7:
            tela_atual = TelaWifi::SenhaRestaurar;
            solicitar_senha_permissao("restaurar a rede principal do firmware");
            return;
        case 8:
            tela_atual = TelaWifi::Politica;
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
    contexto_wifi.politica = servico_wifi_obter_resumo().politica;
    if (opcao == 5) {
        contexto_wifi.politica = {};
        contexto_wifi.campo_politica = CampoPoliticaWifi::RestaurarPadroes;
        tela_atual = TelaWifi::SenhaPolitica;
        solicitar_senha_permissao("restaurar a política Wi-Fi padrão");
        return;
    }
    contexto_wifi.campo_politica =
        static_cast<CampoPoliticaWifi>(opcao - 1);
    tela_atual = TelaWifi::PoliticaValor;
    solicitar_valor_politica_wifi();
}

void tratar_valor_politica_wifi(unsigned long valor) {
    switch (contexto_wifi.campo_politica) {
        case CampoPoliticaWifi::Tentativas:
            if (valor < configuracao::TENTATIVAS_WIFI_MINIMAS ||
                valor > configuracao::TENTATIVAS_WIFI_MAXIMAS) {
                std::printf("\r\nQuantidade de tentativas fora da faixa permitida.\r\n");
                solicitar_valor_politica_wifi();
                return;
            }
            contexto_wifi.politica.tentativas_por_rede =
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
            contexto_wifi.politica.tempo_por_tentativa_ms =
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
            contexto_wifi.politica.intervalo_entre_tentativas_ms =
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
            contexto_wifi.politica.tempo_limite_sessao_ms =
                static_cast<uint32_t>(valor * 1000UL);
            break;
        case CampoPoliticaWifi::RestaurarPadroes:
            break;
    }

    if (!servico_wifi_politica_valida(contexto_wifi.politica)) {
        std::printf(
            "\r\nValor inválido. Confira a faixa indicada; o limite da sessão "
            "também deve ser igual ou maior que o tempo de uma tentativa.\r\n");
        solicitar_valor_politica_wifi();
        return;
    }
    tela_atual = TelaWifi::SenhaPolitica;
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

    std::printf("\r\nTentando conectar à rede '%s'...\r\n", contexto_wifi.ssid);
    std::fflush(stdout);
    const esp_err_t erro = servico_wifi_conectar_rede_informada(
        contexto_wifi.ssid, contexto_wifi.senha);
    if (erro != ESP_OK) {
        informar_resultado_wifi("", "Não foi possível conectar", erro);
        voltar_menu_wifi();
        return;
    }
    tela_atual = TelaWifi::NovaConfirmarSalvar;
    mostrar_confirmacao_salvar_rede();
}

void tratar_confirmacao_salvar_rede(unsigned long opcao) {
    if (opcao == 1) {
        const esp_err_t erro = servico_wifi_salvar_rede_usuario(
            contexto_wifi.ssid, contexto_wifi.senha);
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
    contexto_wifi.indice_rede = static_cast<size_t>(opcao - 1);
    tela_atual = excluir ? TelaWifi::ConfirmarExclusao
                         : TelaWifi::ConfirmarConexao;
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
        ? servico_wifi_excluir_rede_usuario(contexto_wifi.indice_rede)
        : servico_wifi_conectar_rede_salva(contexto_wifi.indice_rede);
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
    contexto_wifi.automatico = opcao == 1;
    tela_atual = TelaWifi::SenhaAutomatico;
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
        contexto_wifi.ssid, contexto_wifi.senha, contexto_wifi.permissao);
    informar_resultado_wifi("Rede principal alterada e salva com sucesso.",
                           "Não foi possível alterar a rede principal", erro);
    voltar_menu_wifi();
}

void tratar_origem_principal(unsigned long opcao) {
    if (opcao == 1) {
        tela_atual = TelaWifi::PrincipalSsid;
        std::memset(contexto_wifi.ssid, 0, sizeof(contexto_wifi.ssid));
        std::memset(contexto_wifi.senha, 0, sizeof(contexto_wifi.senha));
        imprimir_linha();
        std::printf("NOVA REDE PRINCIPAL\r\n");
        std::printf("Digite o SSID. Enter vazio cancela.\r\n> ");
        std::fflush(stdout);
    } else if (opcao == 2) {
        const ResumoWifi wifi = servico_wifi_obter_resumo();
        if (wifi.quantidade_redes_usuario == 0) {
            std::printf("\r\nNão há redes salvas para escolher.\r\n");
            voltar_menu_wifi();
            return;
        }
        tela_atual = TelaWifi::PrincipalEscolherSalva;
        mostrar_escolha_rede("ESCOLHER REDE PRINCIPAL SALVA");
    } else {
        std::printf("\r\nEscolha 1 ou 2.\r\n");
        mostrar_origem_rede_principal();
    }
}

void tratar_escolha_principal_salva(unsigned long opcao) {
    const ResumoWifi wifi = servico_wifi_obter_resumo();
    if (opcao < 1 || opcao > wifi.quantidade_redes_usuario) {
        std::printf("\r\nRede inválida. Escolha um número da lista.\r\n");
        mostrar_escolha_rede("ESCOLHER REDE PRINCIPAL SALVA");
        return;
    }
    contexto_wifi.indice_rede = static_cast<size_t>(opcao - 1);
    tela_atual = TelaWifi::PrincipalConfirmarSalva;
    mostrar_confirmacao_principal_salva();
}

void tratar_confirmacao_principal_salva(unsigned long opcao) {
    if (opcao == 2) {
        voltar_menu_wifi();
        return;
    }
    if (opcao != 1) {
        std::printf("\r\nOpção inválida.\r\n");
        mostrar_confirmacao_principal_salva();
        return;
    }
    const esp_err_t erro = servico_wifi_definir_rede_salva_como_principal(
        contexto_wifi.indice_rede, contexto_wifi.permissao);
    informar_resultado_wifi(
        "Rede salva testada e definida como principal. Ela saiu da lista de secundárias.",
        "Não foi possível definir a rede salva como principal", erro);
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
        contexto_wifi.permissao);
    informar_resultado_wifi("Rede principal do firmware restaurada com sucesso.",
                           "Não foi possível restaurar a rede principal", erro);
    voltar_menu_wifi();
}

void tratar_texto_wifi(const char* texto) {
    const size_t tamanho = std::strlen(texto);
    if (tela_atual == TelaWifi::NovaSsid ||
        tela_atual == TelaWifi::PrincipalSsid) {
        if (tamanho < 1 || tamanho > 32) {
            std::printf("\r\nO SSID deve ter entre 1 e 32 caracteres.\r\n> ");
            std::fflush(stdout);
            return;
        }
        std::strncpy(contexto_wifi.ssid, texto, sizeof(contexto_wifi.ssid) - 1);
        if (tela_atual == TelaWifi::NovaSsid) {
            tela_atual = TelaWifi::NovaSenha;
            solicitar_senha_rede("SENHA DA NOVA REDE");
        } else {
            tela_atual = TelaWifi::PrincipalSenha;
            solicitar_senha_rede("SENHA DA NOVA REDE PRINCIPAL");
        }
        return;
    }

    if (tela_atual == TelaWifi::NovaSenha ||
        tela_atual == TelaWifi::PrincipalSenha) {
        if ((tamanho != 0 && tamanho < 8) || tamanho > 63) {
            std::printf("\r\nUse senha vazia para rede aberta ou 8 a 63 caracteres.\r\n> ");
            std::fflush(stdout);
            return;
        }
        std::strncpy(contexto_wifi.senha, texto,
                     sizeof(contexto_wifi.senha) - 1);
        if (tela_atual == TelaWifi::NovaSenha) {
            tela_atual = TelaWifi::NovaConfirmarConexao;
            mostrar_confirmacao_nova_rede();
        } else {
            tela_atual = TelaWifi::PrincipalConfirmar;
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

    std::strncpy(contexto_wifi.permissao, texto,
                 sizeof(contexto_wifi.permissao) - 1);
    if (tela_atual == TelaWifi::SenhaAutomatico) {
        const esp_err_t erro = servico_wifi_definir_conexao_automatica(
            contexto_wifi.automatico, contexto_wifi.permissao);
        informar_resultado_wifi(
            contexto_wifi.automatico
                ? "Conexão automática às redes secundárias habilitada."
                : "As redes secundárias voltarão a exigir autorização.",
            "Não foi possível salvar a preferência", erro);
        voltar_menu_wifi();
    } else if (tela_atual == TelaWifi::SenhaPolitica) {
        const esp_err_t erro = servico_wifi_definir_politica(
            contexto_wifi.politica, contexto_wifi.permissao);
        informar_resultado_wifi(
            "Política de conexão Wi-Fi salva com sucesso.",
            "Não foi possível salvar a política de conexão", erro);
        voltar_menu_wifi();
    } else if (tela_atual == TelaWifi::SenhaPrincipal) {
        tela_atual = TelaWifi::PrincipalOrigem;
        mostrar_origem_rede_principal();
    } else {
        tela_atual = TelaWifi::RestaurarConfirmar;
        mostrar_confirmacao_restaurar_principal();
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

struct RotaWifi {
    TelaWifi tela;
    void (*tratar)(unsigned long);
};

constexpr RotaWifi ROTAS_WIFI[] = {
    {TelaWifi::Principal, tratar_menu_wifi},
    {TelaWifi::Politica, tratar_menu_politica_wifi},
    {TelaWifi::PoliticaValor, tratar_valor_politica_wifi},
    {TelaWifi::NovaConfirmarConexao, tratar_confirmacao_nova_rede},
    {TelaWifi::NovaConfirmarSalvar, tratar_confirmacao_salvar_rede},
    {TelaWifi::EscolherConexao, tratar_escolha_conexao},
    {TelaWifi::ConfirmarConexao, tratar_confirmacao_conexao},
    {TelaWifi::EscolherExclusao, tratar_escolha_exclusao},
    {TelaWifi::ConfirmarExclusao, tratar_confirmacao_exclusao},
    {TelaWifi::EscolherAutomatico, tratar_escolha_automatico},
    {TelaWifi::PrincipalOrigem, tratar_origem_principal},
    {TelaWifi::PrincipalEscolherSalva, tratar_escolha_principal_salva},
    {TelaWifi::PrincipalConfirmarSalva, tratar_confirmacao_principal_salva},
    {TelaWifi::PrincipalConfirmar, tratar_confirmacao_principal},
    {TelaWifi::RestaurarConfirmar, tratar_confirmacao_restaurar},
};

}  // namespace

void abrir() {
    tela_atual = TelaWifi::Principal;
    mostrar_menu_wifi();
}

bool voltar() {
    if (tela_atual == TelaWifi::Principal) return false;
    if (tela_atual == TelaWifi::PoliticaValor ||
        tela_atual == TelaWifi::SenhaPolitica) {
        tela_atual = TelaWifi::Politica;
        mostrar_menu_politica_wifi();
    } else {
        voltar_menu_wifi();
    }
    return true;
}

terminal_console::EcoEntrada eco_entrada() {
    using terminal_console::EcoEntrada;
    switch (tela_atual) {
        case TelaWifi::NovaSenha:
        case TelaWifi::PrincipalSenha:
            return EcoEntrada::Silencioso;
        case TelaWifi::SenhaAutomatico:
        case TelaWifi::SenhaPrincipal:
        case TelaWifi::SenhaRestaurar:
        case TelaWifi::SenhaPolitica:
            return EcoEntrada::Mascarado;
        default:
            return EcoEntrada::Normal;
    }
}

bool tratar_texto(const char* texto) {
    if (tela_atual != TelaWifi::NovaSsid &&
        tela_atual != TelaWifi::PrincipalSsid &&
        eco_entrada() == terminal_console::EcoEntrada::Normal) {
        return false;
    }
    tratar_texto_wifi(texto);
    return true;
}

bool tratar_opcao(unsigned long opcao) {
    for (const auto& rota : ROTAS_WIFI) {
        if (rota.tela == tela_atual) {
            rota.tratar(opcao);
            return true;
        }
    }
    return false;
}
}  // namespace terminal_wifi
