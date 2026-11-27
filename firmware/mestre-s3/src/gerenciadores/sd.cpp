// Implementa a montagem transacional de cartão microSD FAT pelo barramento SPI2.
#include "gerenciadores/sd.h"

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "nucleo/configuracao/historico.h"
#include "nucleo/configuracao/placa.h"
#include "sdmmc_cmd.h"

namespace {
constexpr char ETIQUETA[] = "gerenciador_sd";
constexpr spi_host_device_t PORTA_SPI_SD = SPI2_HOST;
constexpr gpio_num_t PINO_CLK = configuracao::PINO_SD_CLK;
constexpr gpio_num_t PINO_MOSI = configuracao::PINO_SD_MOSI;
constexpr gpio_num_t PINO_MISO = configuracao::PINO_SD_MISO;
constexpr gpio_num_t PINO_CS = configuracao::PINO_SD_CS;
bool barramento_iniciado = false;
sdmmc_card_t* cartao = nullptr;

bool pinos_validos_e_distintos() {
    const bool validos =
        GPIO_IS_VALID_OUTPUT_GPIO(PINO_CLK) &&
        GPIO_IS_VALID_OUTPUT_GPIO(PINO_MOSI) &&
        GPIO_IS_VALID_GPIO(PINO_MISO) &&
        GPIO_IS_VALID_OUTPUT_GPIO(PINO_CS);
    const bool distintos =
        PINO_CLK != PINO_MOSI && PINO_CLK != PINO_MISO && PINO_CLK != PINO_CS &&
        PINO_MOSI != PINO_MISO && PINO_MOSI != PINO_CS && PINO_MISO != PINO_CS;
    return validos && distintos;
}

bool vale_tentar_spi_mais_lento(esp_err_t erro) {
    return erro == ESP_FAIL || erro == ESP_ERR_TIMEOUT ||
           erro == ESP_ERR_INVALID_RESPONSE || erro == ESP_ERR_INVALID_CRC;
}
}  // namespace

esp_err_t gerenciador_cartao_sd_encerrar() {
    esp_err_t erro_desmontagem = ESP_OK;
    if (cartao != nullptr) {
        erro_desmontagem = esp_vfs_fat_sdcard_unmount(
            configuracao::PONTO_MONTAGEM_SD, cartao);
        cartao = nullptr;
    }
    if (!barramento_iniciado) return erro_desmontagem;

    const esp_err_t erro_spi = spi_bus_free(PORTA_SPI_SD);
    if (erro_spi == ESP_OK) barramento_iniciado = false;
    return erro_desmontagem != ESP_OK ? erro_desmontagem : erro_spi;
}

esp_err_t gerenciador_cartao_sd_iniciar() {
    if (cartao != nullptr) return ESP_OK;
    constexpr uint32_t frequencia_hz = configuracao::FREQUENCIA_SD_HZ;
    constexpr uint32_t frequencia_alternativa_hz =
        configuracao::FREQUENCIA_SD_ALTERNATIVA_HZ;
    if (configuracao::PONTO_MONTAGEM_SD[0] != '/' ||
        !pinos_validos_e_distintos() || frequencia_hz < 1000 ||
        (frequencia_alternativa_hz != 0 && frequencia_alternativa_hz < 1000) ||
        configuracao::MAXIMO_ARQUIVOS_ABERTOS_SD == 0) {
        ESP_LOGE(ETIQUETA, "Configuração do cartão SD inválida");
        return ESP_ERR_INVALID_ARG;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = PORTA_SPI_SD;
    host.max_freq_khz = frequencia_hz / 1000U;

    spi_bus_config_t barramento{};
    barramento.mosi_io_num = PINO_MOSI;
    barramento.miso_io_num = PINO_MISO;
    barramento.sclk_io_num = PINO_CLK;
    barramento.quadwp_io_num = GPIO_NUM_NC;
    barramento.quadhd_io_num = GPIO_NUM_NC;
    barramento.max_transfer_sz = 4096;

    if (barramento_iniciado) {
        const esp_err_t erro_limpeza = gerenciador_cartao_sd_encerrar();
        if (erro_limpeza != ESP_OK) return erro_limpeza;
    }
    ESP_LOGI(ETIQUETA, "SPI2 SD: CLK=%d MOSI=%d MISO=%d CS=%d, limite=%d kHz",
             PINO_CLK, PINO_MOSI, PINO_MISO, PINO_CS, host.max_freq_khz);
    esp_err_t erro = spi_bus_initialize(PORTA_SPI_SD, &barramento, SPI_DMA_CH_AUTO);
    if (erro != ESP_OK) {
        ESP_LOGE(ETIQUETA, "Falha ao iniciar SPI do cartão: %s",
                 esp_err_to_name(erro));
        return erro;
    }
    barramento_iniciado = true;

    sdspi_device_config_t dispositivo = SDSPI_DEVICE_CONFIG_DEFAULT();
    dispositivo.host_id = PORTA_SPI_SD;
    dispositivo.gpio_cs = PINO_CS;

    esp_vfs_fat_sdmmc_mount_config_t montagem{};
    montagem.format_if_mount_failed = false;
    montagem.max_files = configuracao::MAXIMO_ARQUIVOS_ABERTOS_SD;
    montagem.allocation_unit_size = 16U * 1024U;

    const auto montar = [&]() {
        return esp_vfs_fat_sdspi_mount(configuracao::PONTO_MONTAGEM_SD, &host,
                                      &dispositivo, &montagem, &cartao);
    };
    erro = montar();
    if (vale_tentar_spi_mais_lento(erro) &&
        frequencia_alternativa_hz != 0 &&
        frequencia_alternativa_hz < frequencia_hz) {
        host.max_freq_khz = frequencia_alternativa_hz / 1000U;
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

    ESP_LOGI(ETIQUETA, "Cartão SD montado em %s, limite SPI=%d kHz",
             configuracao::PONTO_MONTAGEM_SD, host.max_freq_khz);
    return ESP_OK;
}
