#pragma once

#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum class ModuloSupervisionado : uint8_t {
    TarefaTelemetria,
    UartEquipe,
    UartVisitantes,
    Quantidade,
};

using AcaoRecuperacaoModulo = esp_err_t (*)();

struct EstadoModuloSupervisionado {
    bool registrado = false;
    bool em_recuperacao = false;
    const char* nome = "-";
    uint32_t tempo_limite_ms = 0;
    uint32_t idade_ultimo_pulso_ms = 0;
    uint32_t falhas_detectadas = 0;
    uint32_t recuperacoes_ok = 0;
    uint32_t recuperacoes_com_falha = 0;
    uint32_t palavras_pilha_livres = 0;
    esp_err_t ultimo_resultado = ESP_OK;
};

esp_err_t servico_supervisao_iniciar();
esp_err_t servico_supervisao_registrar(ModuloSupervisionado modulo, const char* nome,
                                       uint32_t tempo_limite_ms,
                                       AcaoRecuperacaoModulo acao_recuperacao);
void servico_supervisao_alimentar(ModuloSupervisionado modulo);
void servico_supervisao_associar_tarefa(ModuloSupervisionado modulo,
                                        TaskHandle_t tarefa);
EstadoModuloSupervisionado servico_supervisao_obter_estado(
    ModuloSupervisionado modulo);
uint32_t servico_supervisao_obter_menor_pilha_livre();
