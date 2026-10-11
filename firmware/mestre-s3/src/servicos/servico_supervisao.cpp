// Detecta tarefas sem atividade e executa recuperações localizadas.
#include "servicos/servico_supervisao.h"

#include <array>
#include <cstddef>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr char ETIQUETA[] = "supervisao";
constexpr uint32_t INTERVALO_VERIFICACAO_MS = 1000;

struct RegistroModulo {
    EstadoModuloSupervisionado estado;
    AcaoRecuperacaoModulo acao = nullptr;
    uint32_t instante_ultimo_pulso_ms = 0;
    TaskHandle_t tarefa_monitorada = nullptr;
};

std::array<RegistroModulo, static_cast<size_t>(ModuloSupervisionado::Quantidade)> modulos{};
portMUX_TYPE trava = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t tarefa_supervisao = nullptr;
uint32_t menor_pilha_supervisao = 0;

RegistroModulo* localizar(ModuloSupervisionado modulo) {
    const size_t indice = static_cast<size_t>(modulo);
    return indice < modulos.size() ? &modulos[indice] : nullptr;
}

void tarefa(void*) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(INTERVALO_VERIFICACAO_MS));
        const uint32_t pilha_supervisao = uxTaskGetStackHighWaterMark(nullptr);
        const uint32_t agora_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        portENTER_CRITICAL(&trava);
        if (menor_pilha_supervisao == 0 ||
            pilha_supervisao < menor_pilha_supervisao) {
            menor_pilha_supervisao = pilha_supervisao;
        }
        portEXIT_CRITICAL(&trava);
        for (auto& registro : modulos) {
            AcaoRecuperacaoModulo acao = nullptr;
            const char* nome = nullptr;
            portENTER_CRITICAL(&trava);
            if (registro.tarefa_monitorada != nullptr) {
                const uint32_t pilha_atual =
                    uxTaskGetStackHighWaterMark(registro.tarefa_monitorada);
                if (registro.estado.bytes_pilha_livres == 0 ||
                    pilha_atual < registro.estado.bytes_pilha_livres) {
                    registro.estado.bytes_pilha_livres = pilha_atual;
                }
            }
            const uint32_t idade = agora_ms - registro.instante_ultimo_pulso_ms;
            registro.estado.idade_ultimo_pulso_ms = idade;
            if (registro.estado.registrado && registro.estado.em_recuperacao &&
                idade > registro.estado.tempo_limite_ms) {
                registro.estado.em_recuperacao = false;
                registro.estado.recuperacoes_com_falha++;
                registro.estado.ultimo_resultado = ESP_ERR_TIMEOUT;
                registro.instante_ultimo_pulso_ms = agora_ms;
            } else if (registro.estado.registrado &&
                       !registro.estado.em_recuperacao &&
                       idade > registro.estado.tempo_limite_ms) {
                registro.estado.em_recuperacao = true;
                registro.estado.falhas_detectadas++;
                registro.instante_ultimo_pulso_ms = agora_ms;
                acao = registro.acao;
                nome = registro.estado.nome;
            }
            portEXIT_CRITICAL(&trava);
            if (acao == nullptr) continue;

            ESP_LOGE(ETIQUETA, "%s sem atividade; iniciando recuperação localizada", nome);
            const esp_err_t resultado = acao();
            portENTER_CRITICAL(&trava);
            registro.estado.ultimo_resultado = resultado;
            if (resultado != ESP_OK) {
                registro.estado.em_recuperacao = false;
                registro.estado.recuperacoes_com_falha++;
            }
            portEXIT_CRITICAL(&trava);
            if (resultado == ESP_OK) {
                ESP_LOGI(ETIQUETA,
                         "Ação de recuperação de %s executada; aguardando novo pulso",
                         nome);
            } else {
                ESP_LOGE(ETIQUETA, "Falha ao recuperar %s: %s", nome,
                         esp_err_to_name(resultado));
            }
        }
    }
}
}  // namespace

esp_err_t servico_supervisao_iniciar() {
    if (tarefa_supervisao != nullptr) return ESP_OK;
    if (xTaskCreate(tarefa, "supervisao", 3072, nullptr, 8,
                    &tarefa_supervisao) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(ETIQUETA, "Supervisão localizada iniciada");
    return ESP_OK;
}

esp_err_t servico_supervisao_registrar(ModuloSupervisionado modulo, const char* nome,
                                       uint32_t tempo_limite_ms,
                                       AcaoRecuperacaoModulo acao_recuperacao) {
    RegistroModulo* registro = localizar(modulo);
    if (registro == nullptr || nome == nullptr || tempo_limite_ms < 1000 ||
        acao_recuperacao == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&trava);
    registro->estado = {};
    registro->estado.registrado = true;
    registro->estado.nome = nome;
    registro->estado.tempo_limite_ms = tempo_limite_ms;
    registro->acao = acao_recuperacao;
    registro->tarefa_monitorada = nullptr;
    registro->instante_ultimo_pulso_ms =
        static_cast<uint32_t>(esp_timer_get_time() / 1000);
    portEXIT_CRITICAL(&trava);
    return ESP_OK;
}

esp_err_t servico_supervisao_desregistrar(ModuloSupervisionado modulo) {
    RegistroModulo* registro = localizar(modulo);
    if (registro == nullptr) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&trava);
    *registro = {};
    portEXIT_CRITICAL(&trava);
    return ESP_OK;
}

esp_err_t servico_supervisao_alimentar(ModuloSupervisionado modulo) {
    RegistroModulo* registro = localizar(modulo);
    if (registro == nullptr) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&trava);
    const bool registrado = registro->estado.registrado;
    if (registrado) {
        registro->instante_ultimo_pulso_ms =
            static_cast<uint32_t>(esp_timer_get_time() / 1000);
        registro->estado.idade_ultimo_pulso_ms = 0;
        if (registro->estado.em_recuperacao) {
            registro->estado.em_recuperacao = false;
            registro->estado.recuperacoes_ok++;
        }
        registro->estado.ultimo_resultado = ESP_OK;
    }
    portEXIT_CRITICAL(&trava);
    return registrado ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t servico_supervisao_associar_tarefa(ModuloSupervisionado modulo,
                                             TaskHandle_t tarefa) {
    RegistroModulo* registro = localizar(modulo);
    if (registro == nullptr || tarefa == nullptr) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&trava);
    const bool registrado = registro->estado.registrado;
    if (registrado) {
        registro->tarefa_monitorada = tarefa;
        registro->estado.bytes_pilha_livres = 0;
    }
    portEXIT_CRITICAL(&trava);
    return registrado ? ESP_OK : ESP_ERR_INVALID_STATE;
}

EstadoModuloSupervisionado servico_supervisao_obter_estado(
    ModuloSupervisionado modulo) {
    EstadoModuloSupervisionado copia{};
    RegistroModulo* registro = localizar(modulo);
    if (registro == nullptr) return copia;
    const uint32_t agora_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    portENTER_CRITICAL(&trava);
    copia = registro->estado;
    copia.idade_ultimo_pulso_ms = agora_ms - registro->instante_ultimo_pulso_ms;
    portEXIT_CRITICAL(&trava);
    return copia;
}

uint32_t servico_supervisao_obter_menor_pilha_livre() {
    uint32_t menor = 0;
    portENTER_CRITICAL(&trava);
    if (menor_pilha_supervisao != 0) menor = menor_pilha_supervisao;
    for (const auto& registro : modulos) {
        const uint32_t bytes = registro.estado.bytes_pilha_livres;
        if (registro.estado.registrado && bytes != 0 &&
            (menor == 0 || bytes < menor)) {
            menor = bytes;
        }
    }
    portEXIT_CRITICAL(&trava);
    return menor;
}
