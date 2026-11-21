// Implementa a montagem transacional de cartão microSD FAT pelo barramento SPI2.
#include "gerenciadores/sd.h"

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

namespace {
constexpr char ETIQUETA[] = "gerenciador_sd";
constexpr spi_host_device_t PORTA_SPI_SD = SPI2_HOST;
bool barramento_iniciado = false;
sdmmc_card_t* cartao = nullptr;
const char* caminho_montado = nullptr;

bool pinos_validos_e_distintos(const ConfiguracaoCartaoSd& configuracao) {
    const bool validos =
        GPIO_IS_VALID_OUTPUT_GPIO(configuracao.pino_clk) &&
        GPIO_IS_VALID_OUTPUT_GPIO(configuracao.pino_mosi) &&
        GPIO_IS_VALID_GPIO(configuracao.pino_miso) &&
        GPIO_IS_VALID_OUTPUT_GPIO(configuracao.pino_cs);
    const bool distintos =
        configuracao.pino_clk != configuracao.pino_mosi &&
        configuracao.pino_clk != configuracao.pino_miso &&
        configuracao.pino_clk != configuracao.pino_cs &&
        configuracao.pino_mosi != configuracao.pino_miso &&
        configuracao.pino_mosi != configuracao.pino_cs &&
        configuracao.pino_miso != configuracao.pino_cs;
    return validos && distintos;
}

bool vale_tentar_spi_mais_lento(esp_err_t erro) {
    return erro == ESP_FAIL || erro == ESP_ERR_TIMEOUT ||
           erro == ESP_ERR_INVALID_RESPONSE || erro == ESP_ERR_INVALID_CRC;
}
}

esp_err_t gerenciador_cartao_sd_encerrar() {
    esp_err_t erro_desmontagem = ESP_OK;
    if (cartao != nullptr) {
        erro_desmontagem = esp_vfs_fat_sdcard_unmount(caminho_montado, cartao);
        cartao = nullptr;
        caminho_montado = nullptr;
    }
    if (!barramento_iniciado) return erro_desmontagem;

    const esp_err_t erro_spi = spi_bus_free(PORTA_SPI_SD);
    if (erro_spi == ESP_OK) barramento_iniciado = false;
    return erro_desmontagem != ESP_OK ? erro_desmontagem : erro_spi;
}

esp_err_t gerenciador_cartao_sd_iniciar(
    const ConfiguracaoCartaoSd& configuracao) {
    if (cartao != nullptr) return ESP_OK;
    if (configuracao.ponto_montagem == nullptr ||
        configuracao.ponto_montagem[0] != '/' ||
        !pinos_validos_e_distintos(configuracao) ||
        configuracao.frequencia_hz < 1000 ||
        (configuracao.frequencia_alternativa_hz != 0 &&
         configuracao.frequencia_alternativa_hz < 1000) ||
        configuracao.maximo_arquivos_abertos == 0) {
        ESP_LOGE(ETIQUETA, "Configuração do cartão SD inválida");
        return ESP_ERR_INVALID_ARG;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = PORTA_SPI_SD;
    host.max_freq_khz = configuracao.frequencia_hz / 1000U;

    spi_bus_config_t barramento{};
    barramento.mosi_io_num = configuracao.pino_mosi;
    barramento.miso_io_num = configuracao.pino_miso;
    barramento.sclk_io_num = configuracao.pino_clk;
    barramento.quadwp_io_num = GPIO_NUM_NC;
    barramento.quadhd_io_num = GPIO_NUM_NC;
    barramento.max_transfer_sz = 4096;

    if (barramento_iniciado) {
        const esp_err_t erro_limpeza = gerenciador_cartao_sd_encerrar();
        if (erro_limpeza != ESP_OK) return erro_limpeza;
    }
    ESP_LOGI(ETIQUETA, "SPI2 SD: CLK=%d MOSI=%d MISO=%d CS=%d, limite=%d kHz",
             configuracao.pino_clk, configuracao.pino_mosi, configuracao.pino_miso,
             configuracao.pino_cs, host.max_freq_khz);
    esp_err_t erro = spi_bus_initialize(PORTA_SPI_SD, &barramento, SPI_DMA_CH_AUTO);
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Falha ao iniciar SPI do cartão: %s",
                 esp_err_to_name(erro));
        return erro;
    }
    barramento_iniciado = true;

    sdspi_device_config_t dispositivo = SDSPI_DEVICE_CONFIG_DEFAULT();
    dispositivo.host_id = PORTA_SPI_SD;
    dispositivo.gpio_cs = configuracao.pino_cs;

    esp_vfs_fat_sdmmc_mount_config_t montagem{};
    montagem.format_if_mount_failed = false;
    montagem.max_files = configuracao.maximo_arquivos_abertos;
    montagem.allocation_unit_size = 16U * 1024U;

    const auto montar = [&]() {
        return esp_vfs_fat_sdspi_mount(configuracao.ponto_montagem, &host,
                                      &dispositivo, &montagem, &cartao);
    };
    erro = montar();
    if (vale_tentar_spi_mais_lento(erro) &&
        configuracao.frequencia_alternativa_hz != 0 &&
        configuracao.frequencia_alternativa_hz < configuracao.frequencia_hz) {
        host.max_freq_khz = configuracao.frequencia_alternativa_hz / 1000U;
        ESP_LOGW(ETIQUETA, "Montagem falhou (%s); repetindo a %d kHz",
                 esp_err_to_name(erro), host.max_freq_khz);
        erro = montar();
    }
    if (erro != ESP_OK) {
        cartao = nullptr;
        const esp_err_t erro_limpeza = gerenciador_cartao_sd_encerrar();
        if (erro_limpeza != ESP_OK) {
            ESP_LOGW(ETIQUETA, "Falha ao liberar SPI2: %s", esp_err_to_name(erro_limpeza));
        }
        ESP_LOGE(ETIQUETA, "Falha ao montar cartão SD a %d kHz: %s%s",
                 host.max_freq_khz, esp_err_to_name(erro),
                 erro == ESP_FAIL ? " (confira o volume FAT/FAT32 e o log vfs_fat)" : "");
        return erro;
    }

    caminho_montado = configuracao.ponto_montagem;
    ESP_LOGI(ETIQUETA, "Cartão SD montado em %s, limite SPI=%d kHz",
             caminho_montado, host.max_freq_khz);
    return ESP_OK;
}
