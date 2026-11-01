// Protege a imagem recém-instalada durante sua primeira janela de operação.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"

#define GUARDA_OTA_ASSINATURA 0x4F544147u
#define GUARDA_OTA_MAX_REINICIOS_ANORMAIS 3u
#define GUARDA_OTA_TEMPO_CONFIRMACAO_MS (2u * 60u * 1000u)
#define GUARDA_OTA_TEMPO_ESTABILIZACAO_MS (5u * 60u * 1000u)

typedef struct {
    uint32_t assinatura;
    uint32_t endereco_particao;
    uint32_t reinicios_anormais;
    uint32_t integridade;
} guarda_ota_estado_t;

// O estado RTC persiste entre resets, mas não depende de gravações na flash.
static RTC_NOINIT_ATTR guarda_ota_estado_t guarda_ota_estado;

static inline uint32_t guarda_ota_integridade(const guarda_ota_estado_t *estado)
{
    return estado->assinatura ^ estado->endereco_particao ^
           estado->reinicios_anormais ^ 0xA5B64C39u;
}

static inline void guarda_ota_limpar(void)
{
    guarda_ota_estado.assinatura = 0;
    guarda_ota_estado.endereco_particao = 0;
    guarda_ota_estado.reinicios_anormais = 0;
    guarda_ota_estado.integridade = 0;
}

static inline bool guarda_ota_reset_anormal(esp_reset_reason_t motivo)
{
    return motivo == ESP_RST_PANIC || motivo == ESP_RST_INT_WDT ||
           motivo == ESP_RST_TASK_WDT || motivo == ESP_RST_WDT;
}

static inline bool guarda_ota_monitorando(void)
{
    const esp_partition_t *particao = esp_ota_get_running_partition();
    return particao != NULL &&
           guarda_ota_estado.assinatura == GUARDA_OTA_ASSINATURA &&
           guarda_ota_estado.integridade == guarda_ota_integridade(&guarda_ota_estado) &&
           guarda_ota_estado.endereco_particao == particao->address;
}

static inline esp_err_t guarda_ota_solicitar_rollback(const char *etiqueta,
                                                      const char *motivo)
{
    if (!guarda_ota_monitorando()) return ESP_ERR_INVALID_STATE;
    ESP_LOGE(etiqueta, "%s; solicitando retorno ao firmware anterior", motivo);
    const esp_err_t erro = esp_ota_mark_app_invalid_rollback_and_reboot();
    ESP_LOGE(etiqueta, "Rollback pós-OTA indisponível: %s", esp_err_to_name(erro));
    return erro;
}

static inline void guarda_ota_registrar_partida(const char *etiqueta)
{
    const esp_partition_t *particao = esp_ota_get_running_partition();
    esp_ota_img_states_t estado_imagem = ESP_OTA_IMG_UNDEFINED;
    if (particao == NULL) return;
    const bool pendente = esp_ota_get_state_partition(particao, &estado_imagem) == ESP_OK &&
                          estado_imagem == ESP_OTA_IMG_PENDING_VERIFY;
    if (pendente) {
        guarda_ota_estado.assinatura = GUARDA_OTA_ASSINATURA;
        guarda_ota_estado.endereco_particao = particao->address;
        guarda_ota_estado.reinicios_anormais = 0;
        guarda_ota_estado.integridade = guarda_ota_integridade(&guarda_ota_estado);
        ESP_LOGW(etiqueta, "Nova imagem OTA em observação; rollback automático permanece ativo");
        return;
    }
    if (!guarda_ota_monitorando()) {
        guarda_ota_limpar();
        return;
    }
    if (guarda_ota_reset_anormal(esp_reset_reason())) {
        guarda_ota_estado.reinicios_anormais++;
        guarda_ota_estado.integridade = guarda_ota_integridade(&guarda_ota_estado);
        ESP_LOGW(etiqueta, "Reinício anormal pós-OTA: %lu/%u",
                 (unsigned long)guarda_ota_estado.reinicios_anormais,
                 GUARDA_OTA_MAX_REINICIOS_ANORMAIS);
        if (guarda_ota_estado.reinicios_anormais >= GUARDA_OTA_MAX_REINICIOS_ANORMAIS) {
            (void)guarda_ota_solicitar_rollback(
                etiqueta, "Instabilidade pós-OTA recorrente");
        }
    }
}

static inline void guarda_ota_encerrar_observacao(const char *etiqueta)
{
    if (!guarda_ota_monitorando()) return;
    ESP_LOGI(etiqueta, "Imagem OTA estável; observação de reinícios encerrada");
    guarda_ota_limpar();
}
