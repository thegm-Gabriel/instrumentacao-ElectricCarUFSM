// Política única de escalonamento; os serviços continuam donos de suas tarefas.
#pragma once

#include <cstddef>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace configuracao {
struct PoliticaTarefa {
    const char* nome;
    uint32_t pilha_bytes;
    UBaseType_t prioridade;
    BaseType_t nucleo;
};

// Pilhas preservadas até medir os piores casos de TLS, OTA e recuperação.
// Núcleo 1: aquisição/enlaces. Núcleo 0: rede/armazenamento.
inline constexpr PoliticaTarefa TAREFAS[] = {
    {"supervisao", 3072, 8, tskNO_AFFINITY},
    {"recuperacao", 6144, 4, tskNO_AFFINITY},
    {"equipe", 3072, 6, 1},
    {"visitantes", 3072, 6, 1},
    {"uart_tx_equipe", 6144, 7, 1},
    {"uart_tx_visitantes", 6144, 7, 1},
    {"enlace_equipe", 6144, 6, 1},
    {"enlace_visitantes", 6144, 6, 1},
    {"aceleracao", 4096, 6, 1},
    {"telemetria", 6144, 5, 1},
    {"acoes_mestre", 6144, 5, tskNO_AFFINITY},
    {"ota_uart_pipeline", 6144, 5, 1},
    {"servico_ota", 14336, 4, 0},
    {"registro_csv", 10240, 3, 0},
    {"upload_csv", 10240, 3, 0},
    {"terminal", 4096, 3, tskNO_AFFINITY},
    {"sinalizacao", 4096, 3, tskNO_AFFINITY},
    {"painel_local", 4096, 2, tskNO_AFFINITY},
    {"relatorios", 6144, 2, tskNO_AFFINITY},
    {"diagnostico", 6144, 4, tskNO_AFFINITY},
};

inline const PoliticaTarefa* politica_tarefa(const char* nome) {
    if (nome == nullptr) return nullptr;
    for (const auto& politica : TAREFAS) {
        if (std::strcmp(nome, politica.nome) == 0) return &politica;
    }
    return nullptr;
}

// Tarefas criadas pelo ESP-IDF não passam por criar_tarefa(), mas seus tamanhos
// conhecidos também são expostos no diagnóstico.
inline uint32_t pilha_tarefa_sistema(const char* nome) {
    if (nome == nullptr) return 0;
    if (std::strcmp(nome, "main") == 0) return CONFIG_ESP_MAIN_TASK_STACK_SIZE;
    if (std::strcmp(nome, "sys_evt") == 0)
        return CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE;
    return 0;
}

inline BaseType_t criar_tarefa(TaskFunction_t funcao, const char* nome,
                               void* argumento, TaskHandle_t* handle) {
    const auto* politica = politica_tarefa(nome);
    if (politica == nullptr) return pdFAIL;
    return xTaskCreatePinnedToCore(funcao, nome, politica->pilha_bytes,
                                  argumento, politica->prioridade, handle,
                                  politica->nucleo);
}
}  // namespace configuracao
