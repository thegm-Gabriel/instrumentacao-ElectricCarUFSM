// Declara o ciclo de vida do driver Wi-Fi em modo estação.
#pragma once

#include <cstdint>
#include <cstddef>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

struct EstadoWifi {
    bool inicializado = false;
    bool ativo = false;
    bool conectado = false;
    char ssid_atual[33] = {};
    uint32_t tentativas_conexao = 0;
    uint32_t desconexoes = 0;
    uint32_t conexoes_bem_sucedidas = 0;
    int8_t rssi_dbm = 0;
    uint8_t canal = 0;
    int32_t ultimo_motivo_desconexao = 0;
    uint64_t ultima_conexao_ms = 0;
    esp_err_t ultimo_erro = ESP_OK;
};

// Inicializa o driver com o rádio em repouso. Cada chamada de conectar realiza
// somente uma tentativa; repetição e troca de perfil pertencem ao serviço.
esp_err_t gerenciador_wifi_iniciar(const char* ssid, const char* senha);
bool gerenciador_wifi_credenciais_validas(const char* ssid, const char* senha);
esp_err_t gerenciador_wifi_conectar(const char* ssid, const char* senha,
                                    TickType_t tempo_limite);
// Faz uma única varredura antes da sessão; cada posição indica se o SSID foi visto.
// Uma falha de varredura permite ao serviço voltar às tentativas normais.
esp_err_t gerenciador_wifi_procurar_redes(const char* const* ssids,
                                          size_t quantidade, bool* encontradas,
                                          TickType_t tempo_limite);
esp_err_t gerenciador_wifi_desativar(TickType_t tempo_limite);
bool gerenciador_wifi_esta_conectado();
// Getter puro: não consulta o driver nem aguarda mutexes de operação.
EstadoWifi gerenciador_wifi_obter_estado();
