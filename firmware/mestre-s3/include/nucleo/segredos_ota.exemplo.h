// Exemplifica os segredos exigidos para autorizar operações OTA.
#pragma once

/* Copie este arquivo para segredos_ota.h e use uma rede com acesso à internet. */
#define OTA_WIFI_SSID "NOME_DA_REDE"
#define OTA_WIFI_SENHA "SENHA_DA_REDE"

/* Protege a troca da rede principal e o modo de conexão automática. */
#define WIFI_SENHA_PERMISSAO "hrm10"

/*
 * URL /exec do Web App descrito em documentacao/telemetria-google e um token
 * compartilhado escolhido pela equipe.
 */
#define TELEMETRIA_GOOGLE_URL "https://script.google.com/macros/s/SEU_ID/exec"
#define TELEMETRIA_GOOGLE_TOKEN "troque-por-um-token-longo"
