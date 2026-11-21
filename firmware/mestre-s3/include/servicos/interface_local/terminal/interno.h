#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace terminal_console {
enum class EcoEntrada { Normal, Mascarado, Silencioso };

struct OpcaoMenu {
    std::uint8_t numero;
    const char* descricao;
};

template <std::size_t Quantidade>
void imprimir_opcoes(const OpcaoMenu (&opcoes)[Quantidade],
                     const char* recuo = "  ") {
    for (const OpcaoMenu& opcao : opcoes) {
        std::printf("%s[%u] %s\r\n", recuo,
                    static_cast<unsigned>(opcao.numero), opcao.descricao);
    }
}

void imprimir_linha();
void imprimir_titulo(const char* titulo);
void imprimir_prompt(const char* orientacao, const char* rotulo = "comando");
void solicitar_resumo();
}  // namespace terminal_console

namespace terminal_wifi {
void abrir();
// Retorna false quando o terminal deve voltar ao menu principal.
bool voltar();
terminal_console::EcoEntrada eco_entrada();
bool tratar_texto(const char* texto);
bool tratar_opcao(unsigned long opcao);
}  // namespace terminal_wifi

namespace terminal_sensores {
void abrir();
// Retorna false quando o terminal deve voltar ao menu principal.
bool voltar();
bool tratar_opcao(unsigned long opcao);
bool monitorando();
void atualizar_monitor();
}  // namespace terminal_sensores

namespace terminal_operacao {
void abrir_ota();
void abrir_satelites();
// Retorna false quando o terminal deve voltar ao menu principal.
bool voltar();
void tratar_opcao(unsigned long opcao);
}  // namespace terminal_operacao
