// Monta o cartão SD em FAT usando SPI e expõe seu estado ao serviço de registro.
#pragma once

#include "esp_err.h"

esp_err_t gerenciador_cartao_sd_iniciar();
// Chamar somente depois de fechar todos os arquivos e diretórios do cartão.
esp_err_t gerenciador_cartao_sd_encerrar();
