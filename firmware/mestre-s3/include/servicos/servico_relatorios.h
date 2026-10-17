// Declara relatórios periódicos e resumos sob demanda.
#pragma once

#include <cstdint>

#include "dados_telemetria_veiculo.h"
#include "esp_err.h"

enum class GrupoRelatorio : uint8_t {
    Telemetria = 0,
    Uart,
    Sistema,
    Ota,
    Quantidade,
};

// Mantém a formatação e a coleta de diagnósticos fora da tarefa de telemetria.
esp_err_t servico_relatorios_iniciar();
void servico_relatorios_atualizar_dados(const DadosTelemetriaVeiculo& dados);

// A periodicidade pertence ao serviço de relatórios; o terminal apenas a edita.
const char* servico_relatorios_nome_grupo(GrupoRelatorio grupo);
uint32_t servico_relatorios_obter_intervalo(GrupoRelatorio grupo);
esp_err_t servico_relatorios_definir_intervalo(GrupoRelatorio grupo,
                                               uint32_t intervalo_ms);
bool servico_relatorios_alternar_pausa();
bool servico_relatorios_estao_pausados();
void servico_relatorios_solicitar_resumo();
// Solicita o mesmo resumo e aguarda sua impressão antes de o terminal redesenhar.
esp_err_t servico_relatorios_solicitar_resumo_e_aguardar(uint32_t limite_ms);
bool servico_relatorios_deve_exibir(GrupoRelatorio grupo);
