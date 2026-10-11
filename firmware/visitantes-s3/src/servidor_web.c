// Implementa as rotas HTTP e publica a telemetria recebida.
#include "servidor_web.h"

#include <stdio.h>
#include <string.h>

#include "configuracao.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "receptor_uart.h"

static const char *TAG = "web_visitantes";
static const char pagina_html[] =
    "<!doctype html><html lang=pt-BR><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>UFSM Carro | Visitantes</title><style>*{box-sizing:border-box}body{margin:0;background:#10212a;color:#edf5f7;font:16px Arial}main{max-width:850px;margin:auto;padding:24px}h1{margin:0;color:#72d4d6}.sub,.rotulo,footer{color:#a6bec4}.estado,.cartao{background:#1a3540;border-radius:10px;padding:14px}.estado{margin:18px 0}.ok{color:#72e39a}.erro{color:#ff9c9c}.grade{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:12px}.rotulo{font-size:.78rem;text-transform:uppercase}.valor{font-size:1.45rem;font-weight:bold;margin-top:7px}h2{font-size:1.1rem;margin-top:26px}</style>"
    "<main><h1>Telemetria do Veículo</h1><p class=sub>Painel de visitantes · UFSM-CS</p><div id=estado class=estado>Aguardando dados...</div><div class=grade>"
    "<div class=cartao><div class=rotulo>Sequência</div><div class=valor id=sequencia>—</div></div><div class=cartao><div class=rotulo>Tempo mestre</div><div class=valor id=tempo>—</div></div><div class=cartao><div class=rotulo>ADC tensão</div><div class=valor id=tensao>—</div></div><div class=cartao><div class=rotulo>Pacotes válidos</div><div class=valor id=validos>0</div></div></div>"
    "<h2>Dinâmica do veículo</h2><div class=grade><div class=cartao><div class=rotulo>Aceleração longitudinal</div><div class=valor id=longitudinal>—</div></div><div class=cartao><div class=rotulo>Sentido da aceleração</div><div class=valor id=sentido>—</div></div><div class=cartao><div class=rotulo>Aceleração X / Y / Z</div><div class=valor id=acc>—</div></div><div class=cartao><div class=rotulo>Giroscópio X / Y / Z</div><div class=valor id=giro>—</div></div><div class=cartao><div class=rotulo>Temperatura MPU</div><div class=valor id=temperatura>—</div></div><div class=cartao><div class=rotulo>Estado do sensor</div><div class=valor id=mpuEstado>—</div></div></div>"
    "<h2>Comunicação</h2><div class=grade><div class=cartao><div class=rotulo>Pacotes inválidos</div><div class=valor id=invalidos>0</div></div><div class=cartao><div class=rotulo>Falhas de CRC-16</div><div class=valor id=crcs>0</div></div></div><p class=sub>Frente/trás é o sentido do vetor de aceleração no eixo configurado, não o sentido em que o carro se desloca.</p><footer>Dados atualizados a cada 500 ms.</footer></main><script>"
    "function v(i,x){document.getElementById(i).textContent=x}async function a(){try{let d=await(await fetch('/api/telemetria',{cache:'no-store'})).json(),ativo=(d.estado_mpu&1)!==0,cal=(d.estado_mpu&2)!==0,eixoOk=(d.estado_mpu&4)!==0,s=(d.estado_mpu>>3)&3,eixo=['X','Y','Z'][(d.estado_mpu>>5)&3]||'?';v('sequencia',d.sequencia);v('tempo',d.tempo_mestre_ms+' ms');v('tensao',d.tensao_adc_bruta);v('validos',d.pacotes_validos);v('invalidos',d.pacotes_invalidos);v('crcs',d.falhas_crc);v('longitudinal',ativo?(d.aceleracao_longitudinal_milesimos_g/1000).toFixed(3)+' g · '+(d.aceleracao_milesimos_ms2/1000).toFixed(3)+' m/s²':'Indisponível');v('sentido',cal?(s===1?'Para frente':s===2?'Para trás':'Neutro'):'Aguardando');v('acc',ativo?[d.aceleracao_x_milesimos_g,d.aceleracao_y_milesimos_g,d.aceleracao_z_milesimos_g].map(x=>(x/1000).toFixed(3)).join(' / ')+' g':'—');v('giro',ativo?[d.giroscopio_x_centesimos_graus_s,d.giroscopio_y_centesimos_graus_s,d.giroscopio_z_centesimos_graus_s].map(x=>(x/100).toFixed(2)).join(' / ')+' °/s':'—');v('temperatura',ativo?(d.temperatura_mpu_centesimos_c/100).toFixed(1)+' °C':'—');v('mpuEstado',!ativo?'Sem leitura':!eixoOk?'Verificar eixo '+eixo:cal?'Calibrado · eixo '+eixo:'Calibrando · eixo '+eixo);let e=document.getElementById('estado'),ok=d.valido&&d.idade_ms<2000;e.textContent=ok?'Recebendo telemetria':'Sem telemetria recente';e.className='estado '+(ok?'ok':'erro')}catch(e){document.getElementById('estado').textContent='Falha ao consultar painel'}}a();setInterval(a,500)</script>";

static esp_err_t responder_painel(httpd_req_t *requisicao)
{
    httpd_resp_set_type(requisicao, "text/html; charset=utf-8");
    return httpd_resp_send(requisicao, pagina_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t responder_telemetria(httpd_req_t *requisicao)
{
    dados_telemetria_t dados = {0};
    receptor_uart_obter_dados(&dados);
    uint32_t agora = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t idade = dados.ultimo_recebimento_ms == 0 ? UINT32_MAX : agora - dados.ultimo_recebimento_ms;
    char json[1536];
    int tamanho = snprintf(json, sizeof(json),
        "{\"valido\":%s,\"idade_ms\":%lu,\"sequencia\":%u,\"tempo_mestre_ms\":%lu,\"tensao_adc_bruta\":%u,"
        "\"aceleracao_x\":%d,\"aceleracao_y\":%d,\"aceleracao_z\":%d,\"giroscopio_x\":%d,\"giroscopio_y\":%d,\"giroscopio_z\":%d,"
        "\"aceleracao_x_milesimos_g\":%d,\"aceleracao_y_milesimos_g\":%d,\"aceleracao_z_milesimos_g\":%d,"
        "\"giroscopio_x_centesimos_graus_s\":%d,\"giroscopio_y_centesimos_graus_s\":%d,\"giroscopio_z_centesimos_graus_s\":%d,"
        "\"aceleracao_longitudinal_milesimos_g\":%d,\"aceleracao_milesimos_ms2\":%d,\"temperatura_mpu_centesimos_c\":%d,\"estado_mpu\":%u,"
        "\"pacotes_validos\":%lu,\"pacotes_invalidos\":%lu,\"falhas_crc\":%lu,\"falhas_cabecalho\":%lu,\"falhas_conteudo\":%lu,\"pacotes_perdidos\":%lu,\"pacotes_duplicados\":%lu,\"reinicios_mestre\":%lu}",
        dados.valido ? "true" : "false", (unsigned long)idade, dados.sequencia,
        (unsigned long)dados.tempo_mestre_ms, dados.tensao_adc_bruta,
        dados.aceleracao_x, dados.aceleracao_y, dados.aceleracao_z,
        dados.giroscopio_x, dados.giroscopio_y, dados.giroscopio_z,
        dados.aceleracao_x_milesimos_g, dados.aceleracao_y_milesimos_g,
        dados.aceleracao_z_milesimos_g,
        dados.giroscopio_x_centesimos_graus_s,
        dados.giroscopio_y_centesimos_graus_s,
        dados.giroscopio_z_centesimos_graus_s,
        dados.aceleracao_longitudinal_milesimos_g,
        dados.aceleracao_milesimos_ms2, dados.temperatura_mpu_centesimos_c,
        (unsigned)dados.estado_mpu, (unsigned long)dados.pacotes_validos,
        (unsigned long)dados.pacotes_invalidos, (unsigned long)dados.falhas_crc,
        (unsigned long)dados.falhas_cabecalho,
        (unsigned long)dados.falhas_conteudo,
        (unsigned long)dados.pacotes_perdidos,
        (unsigned long)dados.pacotes_duplicados,
        (unsigned long)dados.reinicios_mestre);
    if (tamanho < 0 || tamanho >= (int)sizeof(json)) {
        return httpd_resp_send_err(requisicao,
                                   HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "telemetria indisponivel");
    }
    httpd_resp_set_type(requisicao, "application/json");
    return httpd_resp_send(requisicao, json, tamanho);
}

static void tarefa_dns_cativo(void *argumento)
{
    (void)argumento;
    int soquete = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (soquete < 0) vTaskDelete(NULL);
    struct sockaddr_in endereco = {.sin_family = AF_INET, .sin_port = htons(PORTA_DNS), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(soquete, (struct sockaddr *)&endereco, sizeof(endereco)) < 0) { close(soquete); vTaskDelete(NULL); }
    while (true) {
        uint8_t pacote[512]; struct sockaddr_in cliente; socklen_t tamanho_cliente = sizeof(cliente);
        int tamanho = recvfrom(soquete, pacote, sizeof(pacote), 0, (struct sockaddr *)&cliente, &tamanho_cliente);
        if (tamanho < 17) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        int fim_nome = 12;
        while (fim_nome < tamanho && pacote[fim_nome] != 0) fim_nome += pacote[fim_nome] + 1;
        if (fim_nome + 5 > tamanho) continue;
        int fim_pergunta = fim_nome + 5;
        pacote[2] = 0x81; pacote[3] = 0x80; pacote[6] = 0; pacote[7] = 1; pacote[8] = pacote[9] = pacote[10] = pacote[11] = 0;
        const uint8_t resposta[] = {0xC0,0x0C,0,1,0,1,0,0,0,0,0,4,192,168,4,1};
        if (fim_pergunta + (int)sizeof(resposta) <= (int)sizeof(pacote)) { memcpy(&pacote[fim_pergunta], resposta, sizeof(resposta)); sendto(soquete, pacote, fim_pergunta + sizeof(resposta), 0, (struct sockaddr *)&cliente, tamanho_cliente); }
    }
}

esp_err_t servidor_web_iniciar(void)
{
    httpd_handle_t servidor = NULL;
    httpd_config_t configuracao = HTTPD_DEFAULT_CONFIG();
    configuracao.server_port = PORTA_HTTP;
    ESP_RETURN_ON_ERROR(httpd_start(&servidor, &configuracao), TAG, "servidor HTTP");
    const httpd_uri_t rota_painel = {.uri = "/", .method = HTTP_GET, .handler = responder_painel};
    const httpd_uri_t rota_api = {.uri = "/api/telemetria", .method = HTTP_GET, .handler = responder_telemetria};
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(servidor, &rota_painel), TAG, "rota painel");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(servidor, &rota_api), TAG, "rota API");
    if (xTaskCreate(tarefa_dns_cativo, "dns_visitantes", 4096, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "Painel de visitantes em http://%s", IP_PAINEL);
    return ESP_OK;
}
