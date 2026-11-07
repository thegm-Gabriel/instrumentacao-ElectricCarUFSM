// Estado solicitado em RAM, sem dependência de transporte ou atuadores.
#pragma once
#include "esp_err.h"
#include "controle_remoto.h"

enum class AjusteControleRemoto { Marcha, VelocidadeMaxima, AceleracaoMaxima, Parada };
esp_err_t servico_controle_remoto_definir(AjusteControleRemoto ajuste, uint32_t valor);
estado_controle_remoto_t servico_controle_remoto_obter_estado();
