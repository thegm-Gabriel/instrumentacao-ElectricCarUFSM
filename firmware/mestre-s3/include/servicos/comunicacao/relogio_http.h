#pragma once

#include <ctime>

// Retorna zero enquanto o relógio civil estiver sem UTC válida.
std::time_t relogio_utc_obter();

// Aceita Date de uma resposta HTTPS autenticada para o acerto inicial.
void relogio_http_processar_cabecalho(const char* chave, const char* valor);
