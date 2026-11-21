#include "dispositivos/saidas/gpio_veiculo.h"

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nucleo/configuracao/placa.h"
#include "nucleo/configuracao/veiculo.h"

namespace {
SemaphoreHandle_t mutex_saidas = nullptr;
bool bloqueio_ota = false;
bool bloqueio_falha = false;
bool reversao_permitida = false;
uint32_t revisao_permissao_reversao = 0;
bool rele_ligado = false;
uint8_t ultima_marcha = FAIXA_MARCHA_INDEFINIDA;
uint8_t ultima_reversao = 0;
int64_t liberar_rele_em_us = 0;
constexpr int64_t TEMPO_ACOMODACAO_US = 100000;

esp_err_t desligar() {
    // Desligar a bobina primeiro abre o contato NA antes de mudar marcha/ré.
    esp_err_t erro = gpio_set_level(configuracao::PINO_RELE_HABILITADOR, 0);
    if (erro != ESP_OK) return erro;
    if (rele_ligado)
        liberar_rele_em_us = esp_timer_get_time() + TEMPO_ACOMODACAO_US;
    rele_ligado = false;
    ultima_marcha = FAIXA_MARCHA_INDEFINIDA;
    ultima_reversao = 0;
    erro = gpio_set_level(configuracao::PINO_MARCHA_A, 0);
    if (erro != ESP_OK) return erro;
    erro = gpio_set_level(configuracao::PINO_MARCHA_B, 0);
    if (erro != ESP_OK) return erro;
    return gpio_set_level(configuracao::PINO_REVERSAO, 0);
}
}

esp_err_t saidas_veiculo_iniciar() {
    if (!configuracao::HABILITAR_SAIDAS_VEICULO) return ESP_OK;
    const gpio_num_t pinos[] = {configuracao::PINO_RELE_HABILITADOR,
        configuracao::PINO_MARCHA_A, configuracao::PINO_MARCHA_B,
        configuracao::PINO_REVERSAO};
    uint64_t mascara = 0;
    for (const auto pino : pinos) {
        if (!GPIO_IS_VALID_OUTPUT_GPIO(pino) || (mascara & (1ULL << pino)))
            return ESP_ERR_INVALID_ARG;
        mascara |= 1ULL << pino;
    }
    mutex_saidas = xSemaphoreCreateMutex();
    if (mutex_saidas == nullptr) return ESP_ERR_NO_MEM;
    // Os níveis baixos são preparados antes de tornar os GPIOs saídas.
    for (const auto pino : pinos) {
        const esp_err_t erro = gpio_set_level(pino, 0);
        if (erro != ESP_OK) return erro;
    }
    gpio_config_t config{};
    config.pin_bit_mask = mascara;
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE; // Pull-down físico externo.
    config.intr_type = GPIO_INTR_DISABLE;
    return gpio_config(&config);
}

esp_err_t saidas_veiculo_aplicar(const estado_controle_remoto_t& estado) {
    if (!configuracao::HABILITAR_SAIDAS_VEICULO) return ESP_OK;
    if (mutex_saidas == nullptr) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(mutex_saidas, portMAX_DELAY);
    if (static_cast<int32_t>(estado.revisao - revisao_permissao_reversao) < 0) {
        xSemaphoreGive(mutex_saidas);
        return ESP_OK;
    }
    const bool habilitar = !bloqueio_ota && !bloqueio_falha &&
        estado.partida_concluida &&
        !estado.ota_em_andamento && !estado.parada_solicitada &&
        !estado.corte_por_rele_solicitado && estado.bloqueios == 0 &&
        estado.dac_disponivel &&
        (!estado.reversao_acionar_solicitado ||
         (estado.reversao_permitida && reversao_permitida)) &&
        configuracao::HABILITAR_SAIDA_ACELERADOR_DAC &&
        !(estado.modo_a_aterrar_solicitado && estado.modo_b_aterrar_solicitado);
    const bool mudou = !habilitar || !rele_ligado ||
        ultima_marcha != estado.faixa_marcha_solicitada ||
        ultima_reversao != estado.reversao_acionar_solicitado;
    esp_err_t erro = mudou ? desligar() : ESP_OK;
    if (erro == ESP_OK && habilitar && mudou &&
        esp_timer_get_time() >= liberar_rele_em_us) {
        erro = gpio_set_level(configuracao::PINO_MARCHA_A,
                             estado.modo_a_aterrar_solicitado);
        if (erro == ESP_OK) erro = gpio_set_level(configuracao::PINO_MARCHA_B,
                             estado.modo_b_aterrar_solicitado);
        if (erro == ESP_OK) erro = gpio_set_level(configuracao::PINO_REVERSAO,
                             estado.reversao_acionar_solicitado);
        if (erro == ESP_OK) erro = gpio_set_level(
            configuracao::PINO_RELE_HABILITADOR, 1);
        if (erro != ESP_OK) (void)desligar();
        else {
            rele_ligado = true;
            ultima_marcha = estado.faixa_marcha_solicitada;
            ultima_reversao = estado.reversao_acionar_solicitado;
        }
    }
    xSemaphoreGive(mutex_saidas);
    return erro;
}

esp_err_t saidas_veiculo_definir_permissao_reversao(bool permitir,
                                                  bool parar, uint32_t revisao) {
    if (!configuracao::HABILITAR_SAIDAS_VEICULO) return ESP_OK;
    if (mutex_saidas == nullptr) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(mutex_saidas, portMAX_DELAY);
    esp_err_t erro = ESP_OK;
    if (static_cast<int32_t>(revisao - revisao_permissao_reversao) >= 0) {
        // Impede aplicar um retrato do controle que antecede a troca de permissão.
        revisao_permissao_reversao = revisao;
        reversao_permitida = permitir;
        if (!permitir && (parar || ultima_reversao)) erro = desligar();
    }
    xSemaphoreGive(mutex_saidas);
    return erro;
}

esp_err_t saidas_veiculo_bloquear_ota() {
    if (!configuracao::HABILITAR_SAIDAS_VEICULO) return ESP_OK;
    if (mutex_saidas == nullptr) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(mutex_saidas, portMAX_DELAY);
    bloqueio_ota = true;
    const esp_err_t erro = desligar();
    xSemaphoreGive(mutex_saidas);
    return erro;
}

void saidas_veiculo_encerrar_ota() {
    if (!configuracao::HABILITAR_SAIDAS_VEICULO || mutex_saidas == nullptr)
        return;
    xSemaphoreTake(mutex_saidas, portMAX_DELAY);
    bloqueio_ota = false;
    xSemaphoreGive(mutex_saidas);
}

void saidas_veiculo_bloquear_falha() {
    if (!configuracao::HABILITAR_SAIDAS_VEICULO || mutex_saidas == nullptr)
        return;
    xSemaphoreTake(mutex_saidas, portMAX_DELAY);
    bloqueio_falha = true;
    (void)desligar();
    xSemaphoreGive(mutex_saidas);
}
