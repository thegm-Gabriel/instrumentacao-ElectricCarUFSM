// Implementa a montagem transacional de cartão microSD FAT pelo barramento SPI2.
#include "gerenciadores/gerenciador_cartao_sd.h"

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

namespace {
constexpr char ETIQUETA[] = "gerenciador_sd";
bool montado = false;
sdmmc_card_t* cartao = nullptr;

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
}

esp_err_t gerenciador_cartao_sd_iniciar(
    const ConfiguracaoCartaoSd& configuracao) {
    if (montado) return ESP_OK;
    if (configuracao.ponto_montagem == nullptr ||
        configuracao.ponto_montagem[0] != '/' ||
        !pinos_validos_e_distintos(configuracao) ||
        configuracao.frequencia_hz < 1000 ||
        configuracao.maximo_arquivos_abertos == 0) {
        ESP_LOGE(ETIQUETA, "Configuração do cartão SD inválida");
        return ESP_ERR_INVALID_ARG;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = configuracao.frequencia_hz / 1000U;

    spi_bus_config_t barramento{};
    barramento.mosi_io_num = configuracao.pino_mosi;
    barramento.miso_io_num = configuracao.pino_miso;
    barramento.sclk_io_num = configuracao.pino_clk;
    barramento.quadwp_io_num = GPIO_NUM_NC;
    barramento.quadhd_io_num = GPIO_NUM_NC;
    barramento.max_transfer_sz = 4096;

    esp_err_t erro = spi_bus_initialize(
        static_cast<spi_host_device_t>(host.slot), &barramento,
        SPI_DMA_CH_AUTO);
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Falha ao iniciar SPI do cartão: %s",
                 esp_err_to_name(erro));
        return erro;
    }

    sdspi_device_config_t dispositivo = SDSPI_DEVICE_CONFIG_DEFAULT();
    dispositivo.host_id = static_cast<spi_host_device_t>(host.slot);
    dispositivo.gpio_cs = configuracao.pino_cs;

    esp_vfs_fat_sdmmc_mount_config_t montagem{};
    montagem.format_if_mount_failed = false;
    montagem.max_files = configuracao.maximo_arquivos_abertos;
    montagem.allocation_unit_size = 16U * 1024U;

    erro = esp_vfs_fat_sdspi_mount(configuracao.ponto_montagem, &host,
                                   &dispositivo, &montagem, &cartao);
    if (erro != ESP_OK) {
        spi_bus_free(static_cast<spi_host_device_t>(host.slot));
        cartao = nullptr;
        ESP_LOGE(ETIQUETA, "Falha ao montar cartão SD: %s",
                 esp_err_to_name(erro));
        return erro;
    }

    montado = true;
    ESP_LOGI(ETIQUETA, "Cartão SD montado em %s", configuracao.ponto_montagem);
    return ESP_OK;
}
