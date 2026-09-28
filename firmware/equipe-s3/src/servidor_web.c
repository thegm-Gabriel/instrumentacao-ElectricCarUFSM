#include "servidor_web.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "configuracao.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gerenciador_uart.h"
#include "lwip/sockets.h"
#include "protocolo_comandos.h"
#include "receptor_uart.h"
#include "servico_comandos_satelite.h"

static const char *TAG = "servidor_web";

static const char pagina_html[] =
    "<!doctype html><html lang=pt-BR><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>UFSM Carro | Telemetria</title><style>"
    "*{box-sizing:border-box}body{margin:0;background:#101720;color:#e8edf2;font:16px Arial,sans-serif}"
    "main{max-width:920px;margin:auto;padding:24px}h1{margin:0;color:#f5b942;font-size:1.7rem}.sub{color:#9baaba;margin:7px 0 22px}"
    ".estado{padding:10px 14px;border-radius:8px;background:#263544;margin-bottom:18px}.ok{color:#65db8a}.erro{color:#ff8080}"
    ".grade{display:grid;grid-template-columns:repeat(auto-fit,minmax(175px,1fr));gap:12px}.cartao{background:#1b2633;padding:16px;border-radius:10px}"
    ".rotulo{color:#9baaba;font-size:.8rem;text-transform:uppercase}.valor{font-size:1.5rem;margin-top:7px;font-weight:bold}"
    "footer{color:#9baaba;font-size:.85rem;margin-top:22px}</style><main><h1>Telemetria do Veículo</h1>"
    "<p class=sub>UFSM-CS · ESP32-S3 Equipe</p><div id=estado class=estado>Aguardando dados da UART...</div>"
    "<div class=grade><div class=cartao><div class=rotulo>Velocidade</div><div class=valor id=velocidade>—</div></div>"
    "<div class=cartao><div class=rotulo>Marcha</div><div class=valor id=marcha>—</div></div>"
    "<div class=cartao><div class=rotulo>Bateria</div><div class=valor id=carga>—</div></div>"
    "<div class=cartao><div class=rotulo>Tensão / corrente</div><div class=valor id=energia>—</div></div>"
    "<div class=cartao><div class=rotulo>Potência</div><div class=valor id=potencia>—</div></div>"
    "<div class=cartao><div class=rotulo>Acelerador / freio</div><div class=valor id=pedais>—</div></div>"
    "<div class=cartao><div class=rotulo>Autonomia</div><div class=valor id=autonomia>—</div></div>"
    "<div class=cartao><div class=rotulo>Percurso</div><div class=valor id=percurso>—</div></div>"
    "<div class=cartao><div class=rotulo>Células</div><div class=valor id=celulas>—</div></div>"
    "<div class=cartao><div class=rotulo>Proximidade frontal / traseira</div><div class=valor id=proximidade>—</div></div></div>"
    "<h2>Posição e comunicação</h2><div class=grade><div class=cartao><div class=rotulo>Latitude / longitude</div><div class=valor id=posicao>—</div></div>"
    "<div class=cartao><div class=rotulo>Sequência</div><div class=valor id=sequencia>—</div></div>"
    "<div class=cartao><div class=rotulo>Pacotes válidos</div><div class=valor id=validos>0</div></div>"
    "<div class=cartao><div class=rotulo>Pacotes inválidos</div><div class=valor id=invalidos>0</div></div>"
    "<div class=cartao><div class=rotulo>Pacotes perdidos</div><div class=valor id=perdidos>0</div></div>"
    "<div class=cartao><div class=rotulo>Falhas de CRC-16</div><div class=valor id=crcs>0</div></div>"
    "<div class=cartao><div class=rotulo>Taxa de recepção</div><div class=valor id=taxa>—</div></div>"
    "<div class=cartao><div class=rotulo>Intervalo / jitter</div><div class=valor id=jitter>—</div></div>"
    "<div class=cartao><div class=rotulo>Atraso relativo</div><div class=valor id=latencia>—</div></div>"
    "<div class=cartao><div class=rotulo>Perda estimada</div><div class=valor id=perda>—</div></div>"
    "<div class=cartao><div class=rotulo>Idade da amostra</div><div class=valor id=idade>—</div></div></div>"
    "<footer>Atualização automática a cada 500 ms. · <a href=/diagnostico style='color:#f5b942'>Diagnóstico</a> · <a href=/mestre style='color:#f5b942'>Mestre remoto</a></footer></main><script>"
    "function v(id,x){document.getElementById(id).textContent=x}"
    "async function atualizar(){try{let d=await (await fetch('/api/telemetria',{cache:'no-store'})).json();"
    "let m=['P','N','D','R'];v('velocidade',(d.velocidade_centesimos_kmh/100).toFixed(1)+' km/h');v('marcha',m[d.marcha]||'?');v('carga',(d.carga_decimos_percentual/10).toFixed(1)+'%');v('energia',(d.tensao_pacote_mv/1000).toFixed(2)+' V / '+(d.corrente_centesimos_a/100).toFixed(2)+' A');v('potencia',d.potencia_w+' W');v('pedais',(d.acelerador_decimos_percentual/10).toFixed(1)+'% / '+(d.freio_decimos_percentual/10).toFixed(1)+'%');v('autonomia',(d.autonomia_decimos_km/10).toFixed(1)+' km');v('percurso',(d.percurso_metros/1000).toFixed(3)+' km');v('celulas',d.tensoes_celulas_mv.map(x=>(x/1000).toFixed(3)).join(' / ')+' V');v('proximidade',d.distancias_cm[0]+' / '+d.distancias_cm[4]+' cm');v('posicao',(d.latitude_micrograus/1e6).toFixed(6)+' / '+(d.longitude_micrograus/1e6).toFixed(6));v('sequencia',d.sequencia);v('validos',d.pacotes_validos);v('invalidos',d.pacotes_invalidos);v('perdidos',d.pacotes_perdidos);v('crcs',d.falhas_crc);v('taxa',(d.taxa_pacotes_centesimos_hz/100).toFixed(2)+' pkt/s');v('jitter',d.intervalo_medio_ms+' / '+d.jitter_medio_ms+' ms');v('latencia',d.latencia_relativa_ms+' ms');v('perda',(d.taxa_perda_centesimos_percentual/100).toFixed(2)+'%');v('idade',d.idade_ms+' ms');"
    "let e=document.getElementById('estado');let recente=d.valido&&d.idade_ms<2000;e.textContent=recente?(d.simulado?'Recebendo telemetria simulada':'Recebendo telemetria real'):'Sem telemetria recente pela UART';e.className='estado '+(recente?'ok':'erro')}catch(e){document.getElementById('estado').textContent='Falha ao consultar o painel'}}atualizar();setInterval(atualizar,500);</script>";

static const char pagina_diagnostico_html[] =
    "<!doctype html><html lang=pt-BR><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Diagnóstico | UFSM Carro</title><style>*{box-sizing:border-box}body{margin:0;background:#101720;color:#e8edf2;font:16px Arial}main{max-width:900px;margin:auto;padding:24px}h1{color:#f5b942}.grade{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:12px}.c{background:#1b2633;padding:15px;border-radius:10px}.r{color:#9baaba;font-size:.78rem;text-transform:uppercase}.v{font-size:1.25rem;font-weight:bold;margin-top:7px}.ok{color:#65db8a}.erro{color:#ff8080}a{color:#f5b942}</style><main>"
    "<h1>Diagnóstico do satélite da equipe</h1><p><a href=/>← Voltar ao painel</a> · <a href=/mestre>Mestre remoto</a></p><div class=grade>"
    "<div class=c><div class=r>Estado do enlace</div><div class=v id=estado>—</div></div>"
    "<div class=c><div class=r>Firmware / protocolo</div><div class=v id=versao>—</div></div>"
    "<div class=c><div class=r>Tempo ativo</div><div class=v id=uptime>—</div></div>"
    "<div class=c><div class=r>Heap livre / mínimo</div><div class=v id=heap>—</div></div>"
    "<div class=c><div class=r>Clientes Wi-Fi</div><div class=v id=clientes>—</div></div>"
    "<div class=c><div class=r>Taxa / intervalo</div><div class=v id=taxa>—</div></div>"
    "<div class=c><div class=r>Jitter / atraso relativo</div><div class=v id=atraso>—</div></div>"
    "<div class=c><div class=r>Pacotes válidos / inválidos</div><div class=v id=pacotes>—</div></div>"
    "<div class=c><div class=r>Perdidos / duplicados</div><div class=v id=sequencia>—</div></div>"
    "<div class=c><div class=r>CRC / cabeçalho / conteúdo</div><div class=v id=erros>—</div></div>"
    "<div class=c><div class=r>UART RX / TX</div><div class=v id=bytes>—</div></div>"
    "<div class=c><div class=r>Erros físicos UART</div><div class=v id=fisicos>—</div></div>"
    "<div class=c><div class=r>Comandos ao mestre</div><div class=v id=comandos>—</div></div></div>"
    "<script>function v(i,x){document.getElementById(i).textContent=x}async function a(){try{let d=await(await fetch('/api/diagnostico',{cache:'no-store'})).json();let ok=d.valido&&d.idade_ms<2000;v('estado',ok?'Recebendo · '+d.idade_ms+' ms':'Sem dados recentes');document.getElementById('estado').className='v '+(ok?'ok':'erro');v('versao',d.versao_firmware+' · protocolo v'+d.versao_protocolo);v('uptime',(d.tempo_ativo_ms/1000).toFixed(0)+' s');v('heap',(d.heap_livre/1024).toFixed(1)+' / '+(d.heap_minimo/1024).toFixed(1)+' KiB');v('clientes',d.clientes_wifi);v('taxa',(d.taxa_pacotes_centesimos_hz/100).toFixed(2)+' pkt/s · '+d.intervalo_medio_ms+' ms');v('atraso',d.jitter_medio_ms+' / '+d.latencia_relativa_ms+' ms');v('pacotes',d.pacotes_validos+' / '+d.pacotes_invalidos);v('sequencia',d.pacotes_perdidos+' / '+d.pacotes_duplicados);v('erros',d.falhas_crc+' / '+d.falhas_cabecalho+' / '+d.falhas_conteudo);v('bytes',d.bytes_recebidos+' / '+d.bytes_enviados+' bytes');v('fisicos',d.erros_fisicos);v('comandos',d.comandos_ok+' respostas · '+d.comandos_timeout+' timeout(s)')}catch(e){v('estado','Falha ao consultar diagnóstico')}}a();setInterval(a,1000)</script>";

static const char pagina_mestre_html[] =
    "<!doctype html><html lang=pt-BR><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Mestre remoto | UFSM Carro</title><style>*{box-sizing:border-box}body{margin:0;background:#101720;color:#e8edf2;font:16px Arial}main{max-width:850px;margin:auto;padding:24px}h1{color:#f5b942}a{color:#f5b942}.acoes{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:10px}button{background:#263544;color:#e8edf2;border:1px solid #40556a;padding:14px;border-radius:8px;text-align:left;font-size:1rem}button:hover{border-color:#f5b942}button:disabled{opacity:.55}.confirmacao{margin:18px 0;padding:16px;border:1px solid #f5b942;border-radius:9px;background:#1b2633}.confirmacao[hidden]{display:none}.confirmacao button{margin:8px 8px 0 0}.estado{padding:10px 13px;border-radius:8px;background:#263544}.processando{color:#f5b942}.sucesso{color:#65db8a}.erro{color:#ff8080}pre{min-height:220px;white-space:pre-wrap;background:#071019;color:#65db8a;padding:16px;border-radius:9px;overflow:auto}</style><main>"
    "<h1>Terminal remoto do mestre</h1><p><a href=/>← Voltar ao painel</a></p>"
    "<p>As consultas são enviadas pela UART somente quando solicitadas.</p><div id=estado class=estado>Pronto para enviar comandos.</div><h2>Consultas</h2><div class=acoes>"
    "<button onclick=enviar('ping')>[1] Ping e latência</button><button onclick=enviar('versao')>[2] Versão e capacidades</button>"
    "<button onclick=enviar('saude')>[3] Saúde do sistema</button><button onclick=enviar('telemetria')>[4] Última telemetria</button>"
    "<button onclick=enviar('uart')>[5] Estado da UART</button><button onclick=enviar('ota')>[6] Estado da OTA</button>"
    "<button onclick=enviar('wifi')>[7] Estado do Wi-Fi</button></div><h2>Ações confirmadas</h2><div class=acoes>"
    "<button onclick=\"preparar('verificar_ota','Solicitar verificação de atualizações?')\">[8] Verificar atualizações</button>"
    "<button onclick=\"preparar('autorizar_mestre','Autorizar a OTA do mestre?')\">[9] Autorizar OTA do mestre</button>"
    "<button onclick=\"preparar('autorizar_equipe','Autorizar a OTA do satélite da equipe?')\">[10] Autorizar OTA da equipe</button>"
    "<button onclick=\"preparar('autorizar_visitantes','Autorizar a OTA do satélite de visitantes?')\">[11] Autorizar OTA de visitantes</button>"
    "<button onclick=\"preparar('cancelar_ota','Cancelar as atualizações pendentes?')\">[12] Cancelar OTA pendente</button>"
    "<button onclick=\"preparar('resumo_terminal','Solicitar um resumo no terminal serial do mestre?')\">[13] Resumo no terminal do mestre</button></div>"
    "<div id=confirmacao class=confirmacao hidden><strong id=pergunta></strong><br><button onclick=confirmar()>Confirmar</button><button onclick=cancelar()>Voltar</button></div>"
    "<h2>Resposta</h2><pre id=saida>Escolha uma consulta.</pre></main>"
    "<script>let pendente='',cicloOta=0,ocupado=false;const saida=document.getElementById('saida'),estado=document.getElementById('estado'),caixa=document.getElementById('confirmacao');function mensagem(t,c){estado.textContent=t;estado.className='estado '+c}function bloquear(v){ocupado=v;document.querySelectorAll('button').forEach(b=>b.disabled=v)}function preparar(c,m){if(ocupado)return;pendente=c;document.getElementById('pergunta').textContent=m;caixa.hidden=false;mensagem('Confirme a ação abaixo.','processando');caixa.scrollIntoView()}function cancelar(){pendente='';caixa.hidden=true;mensagem('Ação cancelada.','')}async function enviar(c,metodo='GET',silencioso=false){if(ocupado&&!silencioso)return null;caixa.hidden=true;if(!silencioso){bloquear(true);saida.className='';saida.textContent='Enviando '+c+' ao mestre...';mensagem('Aguardando resposta do mestre pela UART...','processando')}let controlador=new AbortController(),limite=setTimeout(()=>controlador.abort(),12000);try{let r=await fetch('/api/mestre?consulta='+encodeURIComponent(c),{method:metodo,cache:'no-store',signal:controlador.signal});let texto=await r.text(),d;try{d=JSON.parse(texto)}catch(_){throw new Error('resposta HTTP inválida: '+texto)}saida.textContent=JSON.stringify(d,null,2);if(!r.ok||d.ok===false){saida.className='erro';mensagem(d.erro||'Comando recusado pelo mestre.','erro')}else{saida.className='';mensagem('Resposta recebida com sucesso.','sucesso')}return d}catch(e){saida.textContent='Falha ao comunicar com o mestre: '+(e.name==='AbortError'?'tempo limite excedido':e.message);saida.className='erro';mensagem('Não foi possível concluir o comando.','erro');return null}finally{clearTimeout(limite);if(!silencioso)bloquear(false)}}async function confirmar(){if(!pendente)return;let c=pendente;pendente='';caixa.hidden=true;let d=await enviar(c,'POST');if(d&&d.ok&&c!=='resumo_terminal'){let meu=++cicloOta;acompanharOta(meu)}}async function acompanharOta(meu){for(let i=0;i<90&&meu===cicloOta;i++){await new Promise(r=>setTimeout(r,2000));let d=await enviar('ota','GET',true);if(!d||!d.ok)return;if(i>0&&![2,3,6].includes(d.estado))return}}</script></html>";

static esp_err_t responder_painel(httpd_req_t *requisicao)
{
    httpd_resp_set_type(requisicao, "text/html; charset=utf-8");
    return httpd_resp_send(requisicao, pagina_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t responder_mestre_pagina(httpd_req_t *requisicao)
{
    httpd_resp_set_hdr(requisicao, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_type(requisicao, "text/html; charset=utf-8");
    return httpd_resp_send(requisicao, pagina_mestre_html, HTTPD_RESP_USE_STRLEN);
}

static void escapar_texto_json(char *destino, size_t capacidade,
                               const char *origem)
{
    if (destino == NULL || capacidade == 0) return;
    size_t saida = 0;
    for (size_t entrada = 0; origem != NULL && origem[entrada] != '\0' &&
                            saida + 1 < capacidade; ++entrada) {
        const unsigned char caractere = (unsigned char)origem[entrada];
        if ((caractere == '"' || caractere == '\\') && saida + 2 < capacidade) {
            destino[saida++] = '\\';
            destino[saida++] = (char)caractere;
        } else if (caractere >= 0x20) {
            destino[saida++] = (char)caractere;
        }
    }
    destino[saida] = '\0';
}

static codigo_comando_t localizar_consulta_mestre(const char *consulta)
{
    if (strcmp(consulta, "ping") == 0) return COMANDO_PING;
    if (strcmp(consulta, "versao") == 0) return COMANDO_OBTER_VERSAO;
    if (strcmp(consulta, "saude") == 0) return COMANDO_OBTER_SAUDE;
    if (strcmp(consulta, "telemetria") == 0) return COMANDO_OBTER_TELEMETRIA;
    if (strcmp(consulta, "uart") == 0) return COMANDO_OBTER_ESTADO_UART;
    if (strcmp(consulta, "ota") == 0) return COMANDO_OBTER_ESTADO_OTA;
    if (strcmp(consulta, "wifi") == 0) return COMANDO_OBTER_ESTADO_WIFI;
    if (strcmp(consulta, "verificar_ota") == 0)
        return COMANDO_SOLICITAR_VERIFICACAO_OTA;
    if (strcmp(consulta, "autorizar_mestre") == 0)
        return COMANDO_AUTORIZAR_OTA_MESTRE;
    if (strcmp(consulta, "autorizar_equipe") == 0)
        return COMANDO_AUTORIZAR_OTA_EQUIPE;
    if (strcmp(consulta, "autorizar_visitantes") == 0)
        return COMANDO_AUTORIZAR_OTA_VISITANTES;
    if (strcmp(consulta, "cancelar_ota") == 0) return COMANDO_CANCELAR_OTA;
    if (strcmp(consulta, "resumo_terminal") == 0)
        return COMANDO_SOLICITAR_RESUMO_TERMINAL;
    return 0;
}

static bool comando_exige_confirmacao(codigo_comando_t comando)
{
    switch (comando) {
        case COMANDO_SOLICITAR_VERIFICACAO_OTA:
        case COMANDO_AUTORIZAR_OTA_MESTRE:
        case COMANDO_AUTORIZAR_OTA_EQUIPE:
        case COMANDO_AUTORIZAR_OTA_VISITANTES:
        case COMANDO_CANCELAR_OTA:
        case COMANDO_SOLICITAR_RESUMO_TERMINAL:
            return true;
        default:
            return false;
    }
}

static const char *descrever_resultado_comando(
    codigo_resposta_comando_t resultado)
{
    switch (resultado) {
        case RESPOSTA_COMANDO_OK: return "comando aceito";
        case RESPOSTA_COMANDO_NAO_SUPORTADO: return "comando não suportado";
        case RESPOSTA_COMANDO_CARGA_INVALIDA: return "dados do comando inválidos";
        case RESPOSTA_COMANDO_OCUPADO:
            return "operação indisponível no estado atual";
        case RESPOSTA_COMANDO_NEGADO: return "permissão negada";
        case RESPOSTA_COMANDO_ERRO_INTERNO: return "erro interno no mestre";
        case RESPOSTA_COMANDO_TIMEOUT: return "tempo de resposta esgotado";
    }
    return "resultado desconhecido";
}

static esp_err_t enviar_json_comando(httpd_req_t *requisicao,
                                     const char *estado_http,
                                     const char *json)
{
    if (estado_http != NULL) httpd_resp_set_status(requisicao, estado_http);
    httpd_resp_set_hdr(requisicao, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_type(requisicao, "application/json");
    return httpd_resp_send(requisicao, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t responder_mestre_api(httpd_req_t *requisicao)
{
    char consulta_url[64] = {0};
    char consulta[32] = {0};
    if (httpd_req_get_url_query_str(requisicao, consulta_url,
                                    sizeof(consulta_url)) != ESP_OK ||
        httpd_query_key_value(consulta_url, "consulta", consulta,
                              sizeof(consulta)) != ESP_OK) {
        return enviar_json_comando(requisicao, "400 Bad Request",
                                   "{\"ok\":false,\"erro\":\"consulta ausente\"}");
    }
    const codigo_comando_t comando = localizar_consulta_mestre(consulta);
    if (comando == 0) {
        return enviar_json_comando(requisicao, "400 Bad Request",
                                   "{\"ok\":false,\"erro\":\"consulta desconhecida\"}");
    }
    const bool acao = comando_exige_confirmacao(comando);
    if ((acao && requisicao->method != HTTP_POST) ||
        (!acao && requisicao->method != HTTP_GET)) {
        return enviar_json_comando(requisicao, "405 Method Not Allowed",
            "{\"ok\":false,\"erro\":\"método HTTP não permitido\"}");
    }

    ESP_LOGI(TAG, "Painel solicitou '%s' ao mestre via %s", consulta,
             acao ? "POST" : "GET");

    uint8_t resposta[COMANDO_TAMANHO_MAXIMO_CARGA] = {0};
    uint16_t tamanho_resposta = 0;
    codigo_resposta_comando_t resultado = RESPOSTA_COMANDO_TIMEOUT;
    const int64_t inicio_us = esp_timer_get_time();
    const esp_err_t erro = servico_comandos_satelite_solicitar(
        comando, NULL, 0, resposta, sizeof(resposta), &tamanho_resposta,
        &resultado, pdMS_TO_TICKS(1500));
    if (erro != ESP_OK) {
        if (acao) {
            ESP_LOGW(TAG, "Mestre não confirmou a ação '%s': %s",
                     consulta, esp_err_to_name(erro));
        }
        char json[160];
        snprintf(json, sizeof(json),
                 "{\"ok\":false,\"erro\":\"%s\",\"codigo_esp\":%ld}",
                 esp_err_to_name(erro), (long)erro);
        return enviar_json_comando(requisicao, "504 Gateway Timeout", json);
    }
    if (resultado != RESPOSTA_COMANDO_OK) {
        if (acao) {
            ESP_LOGW(TAG, "Mestre recusou a ação '%s' com resultado %u",
                     consulta, (unsigned)resultado);
        }
        char json[128];
        snprintf(json, sizeof(json),
                 "{\"ok\":false,\"erro\":\"%.63s\",\"resultado\":%u}",
                 descrever_resultado_comando(resultado), (unsigned)resultado);
        return enviar_json_comando(requisicao, "409 Conflict", json);
    }

    char json[512];
    int tamanho = -1;
    if (comando == COMANDO_PING && tamanho_resposta == sizeof(resposta_comando_ping_t)) {
        resposta_comando_ping_t dados;
        memcpy(&dados, resposta, sizeof(dados));
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"ping\",\"rtt_ms\":%lu,\"tempo_ativo_ms\":%lu}",
            (unsigned long)((esp_timer_get_time() - inicio_us) / 1000),
            (unsigned long)dados.tempo_ativo_ms);
    } else if (comando == COMANDO_OBTER_VERSAO &&
               tamanho_resposta == sizeof(resposta_comando_versao_t)) {
        resposta_comando_versao_t dados;
        char versao[65];
        memcpy(&dados, resposta, sizeof(dados));
        dados.versao[sizeof(dados.versao) - 1] = '\0';
        escapar_texto_json(versao, sizeof(versao), dados.versao);
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"versao\",\"firmware\":\"%s\",\"capacidades\":%lu,\"no\":%u}",
            versao, (unsigned long)dados.capacidades, (unsigned)dados.no);
    } else if (comando == COMANDO_OBTER_SAUDE &&
               tamanho_resposta == sizeof(resposta_comando_saude_t)) {
        resposta_comando_saude_t dados;
        memcpy(&dados, resposta, sizeof(dados));
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"saude\",\"estado\":%u,\"causas_ativas\":%lu,\"tempo_ativo_ms\":%lu,\"heap_livre\":%lu,\"erros_historicos\":%lu}",
            (unsigned)dados.estado, (unsigned long)dados.causas_ativas,
            (unsigned long)dados.tempo_ativo_ms, (unsigned long)dados.heap_livre,
            (unsigned long)dados.erros_historicos);
    } else if (comando == COMANDO_OBTER_TELEMETRIA &&
               tamanho_resposta == sizeof(resposta_comando_telemetria_t)) {
        resposta_comando_telemetria_t dados;
        memcpy(&dados, resposta, sizeof(dados));
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"telemetria\",\"sequencia\":%u,\"tempo_mestre_ms\":%lu,\"adc\":%u,\"velocidade_kmh\":%.2f,\"carga_percentual\":%.1f}",
            (unsigned)dados.sequencia, (unsigned long)dados.tempo_mestre_ms,
            (unsigned)dados.tensao_adc_bruta,
            dados.velocidade_centesimos_kmh / 100.0,
            dados.carga_decimos_percentual / 10.0);
    } else if (comando == COMANDO_OBTER_ESTADO_UART &&
               tamanho_resposta == sizeof(resposta_comando_uart_t)) {
        resposta_comando_uart_t dados;
        memcpy(&dados, resposta, sizeof(dados));
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"uart\",\"envios\":%lu,\"recepcoes\":%lu,\"erros\":%lu,\"descartes\":%lu}",
            (unsigned long)dados.envios, (unsigned long)dados.recepcoes,
            (unsigned long)dados.erros, (unsigned long)dados.descartes);
    } else if (comando == COMANDO_OBTER_ESTADO_OTA &&
               tamanho_resposta == sizeof(resposta_comando_ota_t)) {
        resposta_comando_ota_t dados;
        char atual[65], disponivel[65];
        memcpy(&dados, resposta, sizeof(dados));
        dados.versao_atual[sizeof(dados.versao_atual) - 1] = '\0';
        dados.versao_disponivel[sizeof(dados.versao_disponivel) - 1] = '\0';
        escapar_texto_json(atual, sizeof(atual), dados.versao_atual);
        escapar_texto_json(disponivel, sizeof(disponivel), dados.versao_disponivel);
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"ota\",\"estado\":%u,\"alvo\":%u,\"progresso_percentual\":%.1f,\"falhas\":%lu,\"versao_atual\":\"%s\",\"versao_disponivel\":\"%s\"}",
            (unsigned)dados.estado, (unsigned)dados.alvo,
            dados.progresso_decimos / 10.0, (unsigned long)dados.falhas,
            atual, disponivel);
    } else if (comando == COMANDO_OBTER_ESTADO_WIFI &&
               tamanho_resposta == sizeof(resposta_comando_wifi_t)) {
        resposta_comando_wifi_t dados;
        char rede[67];
        memcpy(&dados, resposta, sizeof(dados));
        dados.rede[sizeof(dados.rede) - 1] = '\0';
        escapar_texto_json(rede, sizeof(rede), dados.rede);
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"wifi\",\"radio_ativo\":%s,\"conectado\":%s,\"rssi_dbm\":%d,\"canal\":%u,\"rede\":\"%s\"}",
            dados.radio_ativo ? "true" : "false",
            dados.conectado ? "true" : "false", dados.rssi_dbm,
            (unsigned)dados.canal, rede);
    } else if (acao &&
               tamanho_resposta == sizeof(resposta_comando_acao_t)) {
        resposta_comando_acao_t dados;
        memcpy(&dados, resposta, sizeof(dados));
        if (!dados.aceito || dados.comando != (uint16_t)comando) {
            return enviar_json_comando(requisicao, "502 Bad Gateway",
                "{\"ok\":false,\"erro\":\"confirmação de ação inválida\"}");
        }
        ESP_LOGI(TAG, "Mestre confirmou a ação '%s'", consulta);
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"acao\":\"%s\",\"confirmada\":true,\"tempo_mestre_ms\":%lu,\"mensagem\":\"solicitação aceita; acompanhando o estado\"}",
            consulta, (unsigned long)dados.tempo_mestre_ms);
    } else if (acao && tamanho_resposta == 0) {
        // Compatibilidade durante a atualização gradual: a primeira versão
        // dos comandos confirmava a ação somente pelo código do cabeçalho.
        ESP_LOGW(TAG, "Mestre confirmou a ação '%s' no formato legado",
                 consulta);
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"acao\":\"%s\",\"confirmada\":true,\"formato\":\"legado\",\"mensagem\":\"solicitação aceita; acompanhando o estado\"}",
            consulta);
    }
    if (tamanho < 0 || tamanho >= (int)sizeof(json)) {
        return enviar_json_comando(requisicao, "502 Bad Gateway",
            "{\"ok\":false,\"erro\":\"resposta incompatível\"}");
    }
    return enviar_json_comando(requisicao, NULL, json);
}

static esp_err_t responder_telemetria(httpd_req_t *requisicao)
{
    dados_telemetria_t dados = { 0 };
    receptor_uart_obter_dados(&dados);
    uint32_t agora_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t idade_ms = dados.ultimo_recebimento_ms == 0 ? UINT32_MAX : agora_ms - dados.ultimo_recebimento_ms;
    char json[1536];
    int tamanho = snprintf(json, sizeof(json),
        "{\"valido\":%s,\"idade_ms\":%lu,\"sequencia\":%u,\"tempo_mestre_ms\":%lu,\"tensao_adc_bruta\":%u,"
        "\"aceleracao_x\":%d,\"aceleracao_y\":%d,\"aceleracao_z\":%d,\"giroscopio_x\":%d,\"giroscopio_y\":%d,\"giroscopio_z\":%d,\"pacotes_validos\":%lu,\"pacotes_invalidos\":%lu,"
        "\"marcha\":%u,\"simulado\":%s,\"velocidade_centesimos_kmh\":%u,\"aceleracao_milesimos_ms2\":%d,"
        "\"acelerador_decimos_percentual\":%u,\"freio_decimos_percentual\":%u,\"tensao_pacote_mv\":%u,\"corrente_centesimos_a\":%d,\"carga_decimos_percentual\":%u,"
        "\"tensoes_celulas_mv\":[%u,%u,%u,%u],\"latitude_micrograus\":%ld,\"longitude_micrograus\":%ld,\"rumo_decimos_grau\":%u,"
        "\"distancias_cm\":[%u,%u,%u,%u,%u,%u,%u,%u],\"potencia_w\":%d,\"autonomia_decimos_km\":%u,\"percurso_metros\":%lu,\"odometro_metros\":%lu,"
        "\"falhas_crc\":%lu,\"falhas_cabecalho\":%lu,\"falhas_conteudo\":%lu,\"pacotes_perdidos\":%lu,\"pacotes_duplicados\":%lu,\"reinicios_mestre\":%lu,"
        "\"intervalo_medio_ms\":%lu,\"jitter_medio_ms\":%lu,\"latencia_relativa_ms\":%lu,\"taxa_pacotes_centesimos_hz\":%lu,\"taxa_perda_centesimos_percentual\":%lu}",
        dados.valido ? "true" : "false", (unsigned long)idade_ms,
        (unsigned)dados.sequencia,
        (unsigned long)dados.tempo_mestre_ms, (unsigned)dados.tensao_adc_bruta,
        dados.aceleracao_x,
        dados.aceleracao_y, dados.aceleracao_z, dados.giroscopio_x, dados.giroscopio_y,
        dados.giroscopio_z, (unsigned long)dados.pacotes_validos, (unsigned long)dados.pacotes_invalidos,
        (unsigned)dados.marcha, dados.simulado ? "true" : "false",
        (unsigned)dados.velocidade_centesimos_kmh, dados.aceleracao_milesimos_ms2,
        (unsigned)dados.acelerador_decimos_percentual, (unsigned)dados.freio_decimos_percentual,
        (unsigned)dados.tensao_pacote_mv, dados.corrente_centesimos_a,
        (unsigned)dados.carga_decimos_percentual,
        (unsigned)dados.tensoes_celulas_mv[0], (unsigned)dados.tensoes_celulas_mv[1],
        (unsigned)dados.tensoes_celulas_mv[2], (unsigned)dados.tensoes_celulas_mv[3],
        (long)dados.latitude_micrograus, (long)dados.longitude_micrograus,
        (unsigned)dados.rumo_decimos_grau,
        (unsigned)dados.distancias_cm[0], (unsigned)dados.distancias_cm[1],
        (unsigned)dados.distancias_cm[2], (unsigned)dados.distancias_cm[3],
        (unsigned)dados.distancias_cm[4], (unsigned)dados.distancias_cm[5],
        (unsigned)dados.distancias_cm[6], (unsigned)dados.distancias_cm[7],
        dados.potencia_w,
        (unsigned)dados.autonomia_decimos_km, (unsigned long)dados.percurso_metros,
        (unsigned long)dados.odometro_metros,
        (unsigned long)dados.falhas_crc, (unsigned long)dados.falhas_cabecalho,
        (unsigned long)dados.falhas_conteudo,
        (unsigned long)dados.pacotes_perdidos, (unsigned long)dados.pacotes_duplicados,
        (unsigned long)dados.reinicios_mestre,
        (unsigned long)dados.intervalo_medio_ms,
        (unsigned long)dados.jitter_medio_ms,
        (unsigned long)dados.latencia_relativa_ms,
        (unsigned long)dados.taxa_pacotes_centesimos_hz,
        (unsigned long)dados.taxa_perda_centesimos_percentual);
    if (tamanho < 0 || tamanho >= (int)sizeof(json)) {
        ESP_LOGE(TAG, "JSON de telemetria excedeu o buffer interno");
        return httpd_resp_send_err(requisicao, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "telemetria indisponivel");
    }
    httpd_resp_set_type(requisicao, "application/json");
    return httpd_resp_send(requisicao, json, tamanho);
}

static esp_err_t responder_diagnostico_pagina(httpd_req_t *requisicao)
{
    httpd_resp_set_type(requisicao, "text/html; charset=utf-8");
    return httpd_resp_send(requisicao, pagina_diagnostico_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t responder_diagnostico_api(httpd_req_t *requisicao)
{
    dados_telemetria_t dados = {0};
    estatisticas_gerenciador_uart_t uart = {0};
    const estatisticas_servico_comandos_satelite_t comandos =
        servico_comandos_satelite_obter_estatisticas();
    wifi_sta_list_t clientes = {0};
    receptor_uart_obter_dados(&dados);
    gerenciador_uart_obter_estatisticas(&uart);
    (void)esp_wifi_ap_get_sta_list(&clientes);
    const uint32_t agora_ms = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t idade_ms = dados.ultimo_recebimento_ms == 0
                                  ? UINT32_MAX : agora_ms - dados.ultimo_recebimento_ms;
    char json[1024];
    const int tamanho = snprintf(json, sizeof(json),
        "{\"valido\":%s,\"idade_ms\":%lu,\"versao_firmware\":\"%s\",\"versao_protocolo\":%u,"
        "\"tempo_ativo_ms\":%lu,\"heap_livre\":%lu,\"heap_minimo\":%lu,\"clientes_wifi\":%u,"
        "\"taxa_pacotes_centesimos_hz\":%lu,\"intervalo_medio_ms\":%lu,\"jitter_medio_ms\":%lu,\"latencia_relativa_ms\":%lu,"
        "\"pacotes_validos\":%lu,\"pacotes_invalidos\":%lu,\"pacotes_perdidos\":%lu,\"pacotes_duplicados\":%lu,"
        "\"falhas_crc\":%lu,\"falhas_cabecalho\":%lu,\"falhas_conteudo\":%lu,\"bytes_recebidos\":%llu,\"bytes_enviados\":%llu,\"erros_fisicos\":%lu,"
        "\"comandos_ok\":%lu,\"comandos_timeout\":%lu,\"comandos_invalidos\":%lu}",
        dados.valido ? "true" : "false", (unsigned long)idade_ms,
        esp_app_get_description()->version,
        (unsigned)TELEMETRIA_VERSAO_PROTOCOLO, (unsigned long)agora_ms,
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)esp_get_minimum_free_heap_size(), (unsigned)clientes.num,
        (unsigned long)dados.taxa_pacotes_centesimos_hz,
        (unsigned long)dados.intervalo_medio_ms, (unsigned long)dados.jitter_medio_ms,
        (unsigned long)dados.latencia_relativa_ms,
        (unsigned long)dados.pacotes_validos, (unsigned long)dados.pacotes_invalidos,
        (unsigned long)dados.pacotes_perdidos, (unsigned long)dados.pacotes_duplicados,
        (unsigned long)dados.falhas_crc, (unsigned long)dados.falhas_cabecalho,
        (unsigned long)dados.falhas_conteudo,
        (unsigned long long)uart.bytes_recebidos, (unsigned long long)uart.bytes_enviados,
        (unsigned long)(uart.estouros_fifo + uart.buffers_cheios + uart.erros_quadro +
                        uart.erros_paridade + uart.sinais_break),
        (unsigned long)comandos.respostas_recebidas,
        (unsigned long)comandos.timeouts,
        (unsigned long)comandos.quadros_invalidos);
    if (tamanho < 0 || tamanho >= (int)sizeof(json)) {
        return httpd_resp_send_err(requisicao, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "diagnostico indisponivel");
    }
    httpd_resp_set_type(requisicao, "application/json");
    return httpd_resp_send(requisicao, json, tamanho);
}

static void tarefa_dns_cativo(void *argumento)
{
    (void)argumento;
    int soquete = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (soquete < 0) vTaskDelete(NULL);
    struct sockaddr_in endereco = { .sin_family = AF_INET, .sin_port = htons(PORTA_DNS), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(soquete, (struct sockaddr *)&endereco, sizeof(endereco)) < 0) { close(soquete); vTaskDelete(NULL); }
    while (true) {
        uint8_t pacote[512]; struct sockaddr_in cliente; socklen_t tamanho_cliente = sizeof(cliente);
        int tamanho = recvfrom(soquete, pacote, sizeof(pacote), 0, (struct sockaddr *)&cliente, &tamanho_cliente);
        if (tamanho < 17) {
            /* Evita uso total da CPU caso a pilha de rede reporte erro repetidamente. */
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        int fim_nome = 12;
        while (fim_nome < tamanho && pacote[fim_nome] != 0) fim_nome += pacote[fim_nome] + 1;
        if (fim_nome + 5 > tamanho) continue;
        int fim_pergunta = fim_nome + 5;
        pacote[2] = 0x81; pacote[3] = 0x80; pacote[6] = 0; pacote[7] = 1; pacote[8] = pacote[9] = pacote[10] = pacote[11] = 0;
        const uint8_t resposta[] = {0xC0,0x0C,0,1,0,1,0,0,0,0,0,4,192,168,4,1};
        if (fim_pergunta + (int)sizeof(resposta) <= (int)sizeof(pacote)) {
            memcpy(&pacote[fim_pergunta], resposta, sizeof(resposta));
            sendto(soquete, pacote, fim_pergunta + sizeof(resposta), 0, (struct sockaddr *)&cliente, tamanho_cliente);
        }
    }
}

esp_err_t servidor_web_iniciar(void)
{
    httpd_handle_t servidor = NULL;
    httpd_config_t configuracao = HTTPD_DEFAULT_CONFIG();
    configuracao.server_port = PORTA_HTTP;
    ESP_RETURN_ON_ERROR(httpd_start(&servidor, &configuracao), TAG, "servidor HTTP");
    const httpd_uri_t rota_painel = { .uri = "/", .method = HTTP_GET, .handler = responder_painel };
    const httpd_uri_t rota_api = { .uri = "/api/telemetria", .method = HTTP_GET, .handler = responder_telemetria };
    const httpd_uri_t rota_diagnostico = { .uri = "/diagnostico", .method = HTTP_GET, .handler = responder_diagnostico_pagina };
    const httpd_uri_t rota_api_diagnostico = { .uri = "/api/diagnostico", .method = HTTP_GET, .handler = responder_diagnostico_api };
    const httpd_uri_t rota_mestre = { .uri = "/mestre", .method = HTTP_GET, .handler = responder_mestre_pagina };
    const httpd_uri_t rota_api_mestre = { .uri = "/api/mestre", .method = HTTP_GET, .handler = responder_mestre_api };
    const httpd_uri_t rota_api_mestre_acao = { .uri = "/api/mestre", .method = HTTP_POST, .handler = responder_mestre_api };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(servidor, &rota_painel), TAG, "rota painel");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(servidor, &rota_api), TAG, "rota API");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(servidor, &rota_diagnostico), TAG, "rota diagnostico");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(servidor, &rota_api_diagnostico), TAG, "rota API diagnostico");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(servidor, &rota_mestre), TAG, "rota mestre remoto");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(servidor, &rota_api_mestre), TAG, "rota API mestre remoto");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(servidor, &rota_api_mestre_acao), TAG, "rota de ações do mestre");
    if (xTaskCreate(tarefa_dns_cativo, "dns_cativo", 4096, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "Painel disponível em http://%s", IP_PAINEL);
    return ESP_OK;
}
