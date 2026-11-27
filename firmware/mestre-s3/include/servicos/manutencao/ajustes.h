#pragma once

#include "ajustes_desenvolvedor.h"
#include "esp_err.h"

ajustes_desenvolvedor_t ajustes_desenvolvedor_obter();
// Fica apagado durante a partida até a restauração da preferência da NVS.
bool ajustes_desenvolvedor_led_integrado_ativo();
uint32_t ajustes_desenvolvedor_revisao_geometria();
resposta_ajustes_desenvolvedor_t ajustes_desenvolvedor_consultar();
resposta_ajustes_desenvolvedor_extras_t ajustes_desenvolvedor_consultar_extras();
esp_err_t ajustes_desenvolvedor_definir(uint8_t id, int32_t valor);
