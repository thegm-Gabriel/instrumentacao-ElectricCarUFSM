#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "nucleo/configuracao_wifi.h"

struct PoliticaConexaoWifi {
    uint8_t tentativas_por_rede = configuracao::TENTATIVAS_WIFI_PADRAO;
    uint32_t tempo_por_tentativa_ms =
        configuracao::TEMPO_TENTATIVA_WIFI_PADRAO_MS;
    uint32_t intervalo_entre_tentativas_ms =
        configuracao::INTERVALO_TENTATIVAS_WIFI_PADRAO_MS;
    uint32_t tempo_limite_sessao_ms =
        configuracao::TEMPO_LIMITE_SESSAO_WIFI_PADRAO_MS;
};

struct ResumoWifi {
    bool inicializado = false;
    bool radio_ativo = false;
    bool conectado = false;
    bool aguardando_autorizacao_secundaria = false;
    bool principal_alterada = false;
    bool conectar_secundarias_automaticamente = false;
    char rede_ativa[33] = {};
    char rede_principal[33] = {};
    uint8_t quantidade_redes_usuario = 0;
    char redes_usuario[configuracao::MAXIMO_REDES_WIFI_USUARIO][33] = {};
    esp_err_t ultimo_erro = ESP_OK;
    int8_t rssi_dbm = 0;
    uint8_t canal = 0;
    int32_t ultimo_motivo_desconexao = 0;
    uint32_t tentativas_conexao = 0;
    uint32_t conexoes_bem_sucedidas = 0;
    uint32_t desconexoes_inesperadas = 0;
    uint32_t tentativas_ultima_sessao = 0;
    bool sessao_em_andamento = false;
    PoliticaConexaoWifi politica;
};

// Carrega os perfis da NVS já inicializada e tenta primeiro a rede principal.
esp_err_t servico_wifi_iniciar();
esp_err_t servico_wifi_abrir_sessao();
void servico_wifi_encerrar_sessao();

// A chamada representa a autorização explícita do usuário para tentar a rede.
esp_err_t servico_wifi_conectar_rede_informada(const char* ssid, const char* senha);
esp_err_t servico_wifi_conectar_rede_salva(std::size_t indice);

// Salvar é uma decisão separada da conexão para permitir confirmação posterior.
esp_err_t servico_wifi_salvar_rede_usuario(const char* ssid, const char* senha);
esp_err_t servico_wifi_excluir_rede_usuario(std::size_t indice);

// Operações protegidas pela senha administrativa compilada no firmware.
bool servico_wifi_validar_permissao(const char* senha_permissao);
esp_err_t servico_wifi_alterar_rede_principal(const char* ssid, const char* senha,
                                              const char* senha_permissao);
esp_err_t servico_wifi_restaurar_rede_principal(const char* senha_permissao);
esp_err_t servico_wifi_definir_conexao_automatica(bool habilitar,
                                                  const char* senha_permissao);
esp_err_t servico_wifi_definir_politica(const PoliticaConexaoWifi& politica,
                                        const char* senha_permissao);
bool servico_wifi_politica_valida(const PoliticaConexaoWifi& politica);

// Retorna um retrato sem aguardar uma conexão ou alteração em andamento.
ResumoWifi servico_wifi_obter_resumo();
