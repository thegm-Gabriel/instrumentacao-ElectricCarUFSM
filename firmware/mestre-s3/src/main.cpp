// Ponto de composição do mestre. Define a ordem de partida, liga os módulos
// e decide quais falhas permitem operação degradada. A execução periódica e
// o estado de cada módulo ficam nas respectivas tarefas e serviços.
#include <cstdio>

#include "esp_flash.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gerenciadores/guarda_rollback_ota.h"
#include "nvs_flash.h"

#include "nucleo/configuracao/comunicacao.h"
#include "nucleo/configuracao/placa.h"
#include "nucleo/configuracao/veiculo.h"

#include "servicos/comunicacao/comandos_uart.h"
#include "servicos/comunicacao/enlace_uart.h"
#include "servicos/comunicacao/telemetria_uart.h"
#include "servicos/comunicacao/wifi_sessoes.h"
#include "servicos/historico/registro_sd.h"
#include "servicos/interface_local/painel_oled.h"
#include "servicos/interface_local/relatorios.h"
#include "servicos/interface_local/sinalizacao.h"
#include "servicos/interface_local/terminal/console.h"
#include "servicos/manutencao/diagnostico.h"
#include "servicos/manutencao/ota.h"
#include "servicos/manutencao/supervisao.h"
#include "servicos/veiculo/aquisicao.h"
#include "servicos/veiculo/controle.h"

namespace {
constexpr char ETIQUETA[] = "mestre";
void atualizar_partida(const char* etapa, uint8_t percentual,
                       esp_err_t resultado = ESP_OK) {
    const esp_err_t erro_painel =
        servico_painel_local_atualizar_inicializacao(
            etapa, percentual, resultado);
    if (erro_painel != ESP_OK && erro_painel != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(ETIQUETA, "Não foi possível atualizar a etapa '%s' na tela: %s",
                 etapa, esp_err_to_name(erro_painel));
    }
}

void notificar_servico_degradado(const char* servico, esp_err_t erro) {
    // O aviso usa o OLED e a sinalização que estiverem disponíveis.
    char detalhe[22]{};
    std::snprintf(detalhe, sizeof(detalhe), "%.21s", esp_err_to_name(erro));
    const esp_err_t erro_painel = servico_painel_local_notificar(
        TipoMensagemPainelLocal::Aviso, servico, detalhe, 5000);
    if (erro_painel != ESP_OK && erro_painel != ESP_ERR_INVALID_STATE &&
        erro_painel != ESP_ERR_TIMEOUT) {
        ESP_LOGW(ETIQUETA, "Falha ao enfileirar aviso do serviço %s: %s",
                 servico, esp_err_to_name(erro_painel));
    }
    (void)servico_sinalizacao_notificar(EventoSinalizacao::AvisoTemporario);
}

esp_err_t verificar_hardware_n16r8() {
    uint32_t tamanho_flash = 0;
    const esp_err_t erro_flash = esp_flash_get_size(nullptr, &tamanho_flash);
    if (erro_flash != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Falha ao consultar a flash: %s", esp_err_to_name(erro_flash));
        return erro_flash;
    }

    const bool psram_iniciada = esp_psram_is_initialized();
    const size_t tamanho_psram = psram_iniciada ? esp_psram_get_size() : 0;
    ESP_LOGI(ETIQUETA, "Hardware detectado: flash=%lu MB | PSRAM=%lu MB (%s)",
             static_cast<unsigned long>(tamanho_flash / (1024U * 1024U)),
             static_cast<unsigned long>(tamanho_psram / (1024U * 1024U)),
             psram_iniciada ? "ativa" : "inativa");

    // A flash precisa comportar o particionamento, incluindo as imagens OTA.
    // A PSRAM é opcional para a partida e sua ausência é registrada no log.
    if (tamanho_flash != configuracao::TAMANHO_FLASH_ESPERADO_BYTES) {
        ESP_LOGE(ETIQUETA, "Flash inesperada: esperado=16 MB, detectado=%lu MB",
                 static_cast<unsigned long>(tamanho_flash / (1024U * 1024U)));
        return ESP_ERR_INVALID_SIZE;
    }
    if (!psram_iniciada ||
        tamanho_psram != configuracao::TAMANHO_PSRAM_ESPERADO_BYTES) {
        ESP_LOGW(ETIQUETA,
                 "PSRAM inesperada: esperado=8 MB Octal, detectado=%lu MB; recursos opcionais usarão RAM interna",
                 static_cast<unsigned long>(tamanho_psram / (1024U * 1024U)));
    }
    return ESP_OK;
}

esp_err_t inicializar_armazenamento() {
    esp_err_t erro = nvs_flash_init();
    if (erro == ESP_ERR_NVS_NO_FREE_PAGES || erro == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // A NVS também contém odômetro e credenciais. Apagá-la automaticamente
        // para recuperar a inicialização destruiria dados persistentes sem aviso.
        ESP_LOGE(ETIQUETA,
                 "NVS requer manutenção (%s); armazenamento preservado",
                 esp_err_to_name(erro));
    }
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Não foi possível inicializar a NVS: %s",
                 esp_err_to_name(erro));
    }
    return erro;
}

esp_err_t inicializar_aplicacao() {
    // O enlace só começa a receber depois que telemetria e despacho de comandos
    // estiverem prontos. A aquisição recebe aqui o destino dos erros de sensores.
    esp_err_t erro = servico_telemetria_iniciar();
    if (erro != ESP_OK) return erro;
    servico_comandos_definir_despacho(comandos_mestre_despachar);
    erro = servico_enlace_satelites_iniciar();
    if (erro != ESP_OK) return erro;
    return servico_aquisicao_preparar(servico_diagnostico_registrar_resultado);
}

[[noreturn]] void rejeitar_firmware_e_encerrar(esp_err_t erro,
                                               const char* mensagem) {
    // Uma falha crítica aborta a partida. Se houver uma imagem em observação,
    // a guarda OTA pode retornar à versão anterior antes do reinício.
    ESP_LOGE(ETIQUETA, "%s: %s", mensagem, esp_err_to_name(erro));
    atualizar_partida(mensagem, 100, erro);
    const esp_err_t erro_sinalizacao =
        servico_sinalizacao_notificar(EventoSinalizacao::FalhaCritica);
    if (erro_sinalizacao != ESP_OK && erro_sinalizacao != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(ETIQUETA, "Falha ao sinalizar erro crítico: %s",
                 esp_err_to_name(erro_sinalizacao));
    }
    const esp_err_t erro_rejeicao = servico_ota_rejeitar_firmware_em_execucao();
    if (guarda_ota_monitorando()) {
        (void)guarda_ota_solicitar_rollback(ETIQUETA, mensagem);
    }
    if (erro_rejeicao != ESP_OK && erro_rejeicao != ESP_ERR_NOT_FOUND) {
        ESP_LOGE(ETIQUETA, "Falha ao rejeitar firmware pendente: %s",
                 esp_err_to_name(erro_rejeicao));
    }
    ESP_LOGW(ETIQUETA, "Reiniciando para impedir execução parcial do sistema");
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
    while (true) vTaskDelay(portMAX_DELAY);
}
}  // namespace

extern "C" void app_main(void) {
    // Registra o arranque antes do autoteste para acompanhar a imagem em uso.
    const bool ota_revertida = guarda_ota_registrar_partida(ETIQUETA);
    ESP_LOGI(ETIQUETA, "=== Mestre de telemetria UFSM-CS ===");
    ESP_LOGI(ETIQUETA, "Alvo: ESP32-S3-WROOM-1-N16R8");
    ESP_LOGI(ETIQUETA, "ADC GPIO %d | I2C SDA=%d SCL=%d | intervalo=%d ms",
             configuracao::PINO_ACELERADOR_ADC, configuracao::PINO_I2C_SDA,
             configuracao::PINO_I2C_SCL, configuracao::INTERVALO_TELEMETRIA_MS);
    ESP_LOGI(ETIQUETA,
             "Encoders: esquerdo A/B=GPIO%d/%d, direito A/B=GPIO%d/%d",
             configuracao::PINO_ENCODER_ESQUERDO_A,
             configuracao::PINO_ENCODER_ESQUERDO_B,
             configuracao::PINO_ENCODER_DIREITO_A,
             configuracao::PINO_ENCODER_DIREITO_B);
    ESP_LOGW(ETIQUETA, "Fonte de telemetria: %s",
             configuracao::USAR_TELEMETRIA_SIMULADA ? "SIMULADOR" : "SENSORES REAIS");

    esp_err_t erro = verificar_hardware_n16r8();
    if (erro != ESP_OK) {
        rejeitar_firmware_e_encerrar(
            erro, "Hardware incompatível com a configuração N16R8");
        return;
    }

    // A indicação local começa cedo para mostrar o progresso e eventuais falhas.
    erro = servico_sinalizacao_iniciar();
    if (erro != ESP_OK) {
        ESP_LOGW(ETIQUETA, "Sistema seguirá sem LEDs e buzzer: %s",
                  esp_err_to_name(erro));
    }

    const esp_err_t erro_painel = servico_painel_local_iniciar();
    if (erro_painel != ESP_OK) {
        ESP_LOGW(ETIQUETA, "Sistema seguirá sem a tela local: %s",
                  esp_err_to_name(erro_painel));
        (void)servico_sinalizacao_notificar(
            EventoSinalizacao::AvisoTemporario);
    }

    // A base persistente e a supervisão precisam estar prontas antes dos
    // serviços que restauram dados ou registram tarefas supervisionadas.
    atualizar_partida("ARMAZENAMENTO NVS", 10);
    erro = inicializar_armazenamento();
    if (erro != ESP_OK) {
        rejeitar_firmware_e_encerrar(erro, "Falha no armazenamento");
        return;
    }

    atualizar_partida("SUPERVISAO", 20);
    erro = servico_supervisao_iniciar();
    if (erro != ESP_OK) {
        rejeitar_firmware_e_encerrar(erro, "Inicialização da supervisão interrompida");
        return;
    }

    atualizar_partida("SENSORES E ENLACES", 35);
    erro = inicializar_aplicacao();
    if (erro != ESP_OK) {
        rejeitar_firmware_e_encerrar(erro, "Inicialização da aplicação interrompida");
        return;
    }

    erro = servico_controle_remoto_iniciar();
    if (erro != ESP_OK) {
        rejeitar_firmware_e_encerrar(erro, "Controle lógico prioritário indisponível");
        return;
    }

    // O histórico recebe uma amostra a cada 100 ms, em cadência própria.
    // O registrador cuida da escrita e da recuperação do cartão.
    atualizar_partida("CARTAO E HISTORICO", 50);
    const esp_err_t erro_registro = servico_registro_telemetria_iniciar();
    if (erro_registro != ESP_OK && erro_registro != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(ETIQUETA,
                 "Telemetria ao vivo ativa, mas histórico no SD indisponível: %s",
                 esp_err_to_name(erro_registro));
        notificar_servico_degradado("CARTAO SD", erro_registro);
    }

    atualizar_partida("TELEMETRIA", 62);
    erro = servico_telemetria_iniciar_publicacao(
        servico_registro_telemetria_processar_amostra);
    if (erro != ESP_OK) {
        rejeitar_firmware_e_encerrar(
            erro, "Não foi possível iniciar a telemetria supervisionada");
        return;
    }

    // A telemetria local já está ativa. Rede e interfaces são iniciadas a seguir.
    atualizar_partida("REDE WI-FI", 74);
    const esp_err_t erro_wifi = servico_wifi_iniciar();
    if (erro_wifi != ESP_OK) {
        ESP_LOGW(ETIQUETA, "Telemetria ativa, mas Wi-Fi indisponível: %s",
                 esp_err_to_name(erro_wifi));
        notificar_servico_degradado("WI-FI", erro_wifi);
    }

    atualizar_partida("ATUALIZACAO OTA", 84);
    const esp_err_t erro_ota = servico_ota_iniciar();
    if (erro_ota == ESP_OK && ota_revertida)
        servico_ota_informar_validacao_mestre(EstadoValidacaoOta::Revertida);
    if (erro_ota != ESP_OK) {
        ESP_LOGW(ETIQUETA, "Telemetria ativa, mas OTA indisponível: %s",
                 esp_err_to_name(erro_ota));
        notificar_servico_degradado("SERVICO OTA", erro_ota);
    }

    atualizar_partida("TERMINAL", 90);
    erro = servico_terminal_iniciar();
    if (erro != ESP_OK) {
        ESP_LOGW(ETIQUETA, "Sistema ativo, mas menu serial indisponível: %s",
                 esp_err_to_name(erro));
        notificar_servico_degradado("TERMINAL", erro);
    }

    atualizar_partida("RELATORIOS", 94);
    erro = servico_relatorios_iniciar();
    if (erro != ESP_OK) {
        ESP_LOGW(ETIQUETA,
                 "Sistema ativo, mas relatórios periódicos indisponíveis: %s",
                 esp_err_to_name(erro));
        notificar_servico_degradado("RELATORIOS", erro);
    }

    atualizar_partida("DIAGNOSTICO", 97);
    erro = servico_diagnostico_iniciar();
    if (erro != ESP_OK) {
        ESP_LOGW(ETIQUETA, "Sistema ativo, mas diagnóstico periódico indisponível: %s",
                 esp_err_to_name(erro));
        notificar_servico_degradado("DIAGNOSTICO", erro);
    }

    // Wi-Fi e OTA fazem parte do caminho de recuperação remota. Uma imagem
    // ainda pendente não pode ser confirmada se algum deles regrediu.
    if (erro_wifi != ESP_OK || erro_ota != ESP_OK) {
        if (guarda_ota_monitorando()) {
            (void)guarda_ota_solicitar_rollback(
                ETIQUETA, "Autoteste OTA falhou nos serviços de recuperação remota");
        }
        erro = servico_ota_rejeitar_firmware_em_execucao();
        if (erro != ESP_OK) {
            rejeitar_firmware_e_encerrar(
                erro, "Autoteste OTA falhou nos serviços de recuperação remota");
            return;
        }
    }
    const bool observando_ota = guarda_ota_monitorando();
    if (observando_ota)
        servico_ota_informar_validacao_mestre(EstadoValidacaoOta::Validando, GUARDA_OTA_TEMPO_CONFIRMACAO_MS);
    servico_controle_remoto_concluir_partida(observando_ota);
    atualizar_partida("SISTEMA PRONTO", 100);
    (void)servico_painel_local_notificar(
        TipoMensagemPainelLocal::Sucesso, "INICIALIZACAO", "SISTEMA PRONTO",
        4000);
    (void)servico_sinalizacao_notificar(EventoSinalizacao::SistemaPronto);
    if (observando_ota) {
        // A guarda espera a janela inicial para confirmar a imagem e continua
        // observando o firmware pelo restante do prazo de estabilização.
        ESP_LOGI(ETIQUETA,
                 "Observando firmware OTA por %u ms antes da confirmação",
                 GUARDA_OTA_TEMPO_CONFIRMACAO_MS);
        TickType_t prazo = xTaskGetTickCount();
        (void)xTaskDelayUntil(&prazo, pdMS_TO_TICKS(GUARDA_OTA_TEMPO_CONFIRMACAO_MS));
        erro = servico_ota_confirmar_firmware_em_execucao();
        if (erro != ESP_OK) {
            servico_ota_informar_validacao_mestre(EstadoValidacaoOta::Falha);
            rejeitar_firmware_e_encerrar(
                erro, "Não foi possível confirmar o firmware em execução");
            return;
        }
        constexpr uint32_t observacao_restante_ms =
            GUARDA_OTA_TEMPO_ESTABILIZACAO_MS - GUARDA_OTA_TEMPO_CONFIRMACAO_MS;
        servico_ota_informar_validacao_mestre(EstadoValidacaoOta::EmObservacao, observacao_restante_ms);
        (void)xTaskDelayUntil(&prazo, pdMS_TO_TICKS(observacao_restante_ms));
        guarda_ota_encerrar_observacao(ETIQUETA);
        servico_controle_remoto_concluir_partida();
        servico_ota_informar_validacao_mestre(EstadoValidacaoOta::Concluida);
    }
}
