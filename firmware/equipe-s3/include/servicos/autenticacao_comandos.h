#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"

// Credencial local independente da senha compartilhada dos APs.
esp_err_t autenticacao_comandos_iniciar(void);
bool autenticacao_comandos_autorizar(httpd_req_t *requisicao);
