idf_component_register(
    SRCS
        "main.cpp"

        "gerenciadores/i2c.cpp"
        "gerenciadores/indicadores.cpp"
        "gerenciadores/ota_local.cpp"
        "gerenciadores/pcnt.cpp"
        "gerenciadores/sd.cpp"
        "gerenciadores/uart.cpp"
        "gerenciadores/wifi.cpp"

        "dispositivos/entradas/adc.cpp"
        "dispositivos/entradas/chave_marcha.cpp"
        "dispositivos/entradas/encoder.cpp"
        "dispositivos/entradas/ina228.cpp"
        "dispositivos/entradas/mpu6050.cpp"
        "dispositivos/saidas/gpio_veiculo.cpp"
        "dispositivos/saidas/mcp4725.cpp"
        "dispositivos/saidas/ssd1306.cpp"

        "servicos/comunicacao/comandos_mestre.cpp"
        "servicos/comunicacao/comandos_uart.cpp"
        "servicos/comunicacao/enlace_uart.cpp"
        "servicos/comunicacao/relogio_http.cpp"
        "servicos/comunicacao/telemetria_uart.cpp"
        "servicos/comunicacao/wifi_sessoes.cpp"

        "servicos/historico/backup_configuracao.cpp"
        "servicos/historico/registro_sd.cpp"
        "servicos/historico/upload_google.cpp"

        "servicos/interface_local/painel_oled.cpp"
        "servicos/interface_local/relatorios.cpp"
        "servicos/interface_local/sinalizacao.cpp"
        "servicos/interface_local/terminal/console.cpp"
        "servicos/interface_local/terminal/operacoes.cpp"
        "servicos/interface_local/terminal/sensores.cpp"
        "servicos/interface_local/terminal/wifi.cpp"

        "servicos/manutencao/diagnostico.cpp"
        "servicos/manutencao/ota.cpp"
        "servicos/manutencao/ota_satelites.cpp"
        "servicos/manutencao/supervisao.cpp"

        "servicos/veiculo/aceleracao.cpp"
        "servicos/veiculo/aquisicao.cpp"
        "servicos/veiculo/controle.cpp"
        "servicos/veiculo/odometro.cpp"
        "servicos/veiculo/simulador.cpp"
    INCLUDE_DIRS
        "../include"
        "../../compartilhado/protocolos"
    PRIV_REQUIRES
        app_update
        esp_http_client
        esp_https_ota
        esp_netif
        esp_psram
        esp_driver_pcnt
        esp_driver_rmt
        esp_driver_spi
        esp_wifi
        fatfs
        json
        mbedtls
        nvs_flash
        spi_flash)
