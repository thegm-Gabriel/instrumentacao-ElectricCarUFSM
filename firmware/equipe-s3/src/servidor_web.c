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
#include "painel_mestre_web.h"
#include "protocolo_comandos.h"
#include "receptor_uart.h"
#include "servico_comandos_satelite.h"

static const char *TAG = "servidor_web";

static const char pagina_html[] =
    "<!doctype html><html lang='pt-BR'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta name='theme-color' content='#09111a'><title>Painel da equipe | UFSM Carro</title><style>"
    ":root{color-scheme:dark;--fundo:#09111a;--superficie:#111d29;--elevado:#172635;"
    "--borda:#26394a;--texto:#f2f5f7;--fraco:#91a3b3;--amarelo:#f5b942;"
    "--verde:#5ddd91;--vermelho:#ff7480;--azul:#62b7f5;--ciano:#54d2d2}"
    "*{box-sizing:border-box}html{background:var(--fundo)}body{margin:0;min-height:100vh;"
    "background:radial-gradient(circle at 85% 0,#17324a 0,transparent 32rem),var(--fundo);"
    "color:var(--texto);font:15px system-ui,-apple-system,Segoe UI,sans-serif}"
    "body:before{content:'';position:fixed;inset:0;pointer-events:none;opacity:.18;"
    "background-image:linear-gradient(#fff1 1px,transparent 1px),linear-gradient(90deg,#fff1 1px,transparent 1px);background-size:42px 42px;mask-image:linear-gradient(to bottom,#000,transparent 65%)}"
    "main{position:relative;max-width:1240px;margin:auto;padding:22px 24px 34px}"
    ".topo{display:flex;justify-content:space-between;align-items:center;gap:18px;margin-bottom:22px}"
    ".marca{display:flex;align-items:center;gap:13px}.simbolo{display:grid;place-items:center;width:44px;height:44px;border-radius:13px;background:linear-gradient(145deg,#f8c95e,#d89720);color:#171109;font-size:1.15rem;font-weight:900;box-shadow:0 9px 30px #f5b9422b}"
    ".sobretitulo,.rotulo{color:var(--fraco);font-size:.72rem;font-weight:700;letter-spacing:.1em;text-transform:uppercase}"
    "h1{font-size:1.35rem;line-height:1.1;margin:4px 0 0}nav{display:flex;gap:6px;padding:5px;background:#0b151fbb;border:1px solid var(--borda);border-radius:12px;backdrop-filter:blur(10px)}"
    "nav a{color:var(--fraco);text-decoration:none;font-weight:650;padding:9px 13px;border-radius:8px;transition:.18s}nav a:hover{color:var(--texto);background:#ffffff0b}nav a.ativo{color:#171109;background:var(--amarelo)}"
    ".estado{display:flex;align-items:center;justify-content:space-between;gap:16px;margin-bottom:14px;padding:12px 15px;border:1px solid var(--borda);border-radius:12px;background:#0d1823d9;box-shadow:0 12px 32px #0002}"
    ".estado-principal{display:flex;align-items:center;gap:10px;font-weight:700}.ponto{width:9px;height:9px;border-radius:50%;background:var(--fraco);box-shadow:0 0 0 4px #91a3b31c}.estado small{color:var(--fraco);white-space:nowrap}.estado.online{border-color:#2c6446}.estado.online .ponto{background:var(--verde);box-shadow:0 0 0 4px #5ddd9120,0 0 16px #5ddd91}.estado.simulado{border-color:#7a632d}.estado.simulado .ponto{background:var(--amarelo);box-shadow:0 0 0 4px #f5b94220}.estado.offline{border-color:#663740}.estado.offline .ponto{background:var(--vermelho)}"
    ".grade-destaque{display:grid;grid-template-columns:1.35fr repeat(3,minmax(0,1fr));gap:12px;margin-bottom:24px}"
    ".cartao{position:relative;overflow:hidden;background:linear-gradient(150deg,var(--elevado),#101b26);border:1px solid var(--borda);border-radius:15px;padding:17px;box-shadow:0 14px 35px #0002}"
    ".cartao:after{content:'';position:absolute;width:120px;height:120px;border-radius:50%;right:-60px;top:-65px;background:var(--brilho,#62b7f5);opacity:.07;filter:blur(2px)}"
    ".velocidade{min-height:184px;display:flex;flex-direction:column;justify-content:space-between;--brilho:var(--amarelo);background:linear-gradient(145deg,#242516,#121d27 72%)}"
    ".valor-gigante{font-size:clamp(3.2rem,7vw,5.7rem);font-weight:760;line-height:.9;letter-spacing:-.07em}.unidade{font-size:.9rem;color:var(--fraco);font-weight:650;letter-spacing:0}.fonte{display:inline-flex;align-items:center;gap:6px;color:var(--fraco);font-size:.78rem}.fonte:before{content:'';width:6px;height:6px;border-radius:50%;background:var(--azul)}"
    ".valor{font-size:1.7rem;font-weight:750;margin-top:11px;line-height:1.08;letter-spacing:-.025em}.detalhe{color:var(--fraco);font-size:.82rem;line-height:1.4;margin-top:7px}.linha-detalhes{display:flex;gap:12px;flex-wrap:wrap;color:var(--fraco);font-size:.82rem;margin-top:9px}.linha-detalhes b{color:var(--texto)}"
    ".barra{height:7px;margin-top:14px;background:#071019;border-radius:99px;overflow:hidden}.barra span{display:block;width:0;height:100%;border-radius:inherit;background:linear-gradient(90deg,var(--ciano),var(--verde));transition:width .35s ease}.barra.amarela span{background:linear-gradient(90deg,#d69520,var(--amarelo))}.barra.vermelha span{background:linear-gradient(90deg,#ac3847,var(--vermelho))}"
    ".titulo-secao{display:flex;justify-content:space-between;align-items:end;gap:12px;margin:0 0 11px}.titulo-secao h2{font-size:1rem;margin:0}.titulo-secao p{color:var(--fraco);font-size:.8rem;margin:0}.grade-operacao{display:grid;grid-template-columns:repeat(12,1fr);gap:12px;margin-bottom:24px}.marcha{grid-column:span 2}.pedais{grid-column:span 4}.proximidade{grid-column:span 3}.celulas{grid-column:span 3}.posicao{grid-column:span 5}.comunicacao{grid-column:span 7}"
    ".marcha .valor{font-size:3.4rem;color:var(--amarelo)}.pedal{display:grid;grid-template-columns:76px 1fr 48px;align-items:center;gap:9px;margin-top:13px;font-size:.8rem}.pedal .barra{margin:0}.pedal b{text-align:right}.duplo{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:15px}.mini{background:#0c1721;border:1px solid #203343;border-radius:10px;padding:11px}.mini strong{display:block;font-size:1.2rem;margin-top:5px}.lista-celulas{display:flex;flex-wrap:wrap;gap:7px;margin-top:13px}.celula{background:#0b1620;border:1px solid #263d50;color:#c6d4df;border-radius:7px;padding:6px 8px;font:650 .76rem ui-monospace,SFMono-Regular,monospace}"
    ".coordenadas{font:650 1.08rem ui-monospace,SFMono-Regular,monospace;line-height:1.55;margin-top:10px}.metricas-link{display:grid;grid-template-columns:repeat(4,1fr);gap:1px;background:var(--borda);border:1px solid var(--borda);border-radius:11px;overflow:hidden;margin-top:13px}.metrica-link{background:#0c1721;padding:11px}.metrica-link span{display:block;color:var(--fraco);font-size:.68rem;text-transform:uppercase;letter-spacing:.07em}.metrica-link b{display:block;margin-top:5px;font-size:1rem}.alerta{color:var(--vermelho)!important}.bom{color:var(--verde)!important}"
    "footer{display:flex;justify-content:space-between;gap:12px;color:var(--fraco);font-size:.78rem;padding-top:2px}footer span:last-child{text-align:right}"
    ".offline .cartao{filter:saturate(.72)}@media(max-width:980px){.grade-destaque{grid-template-columns:1.25fr 1fr 1fr}.grade-destaque .percurso-card{grid-column:1/-1}.marcha{grid-column:span 3}.pedais{grid-column:span 5}.proximidade{grid-column:span 4}.celulas{grid-column:span 5}.posicao{grid-column:span 7}.comunicacao{grid-column:1/-1}}"
    "@media(max-width:680px){main{padding:16px 14px 28px}.topo{align-items:flex-start;flex-direction:column}nav{width:100%;overflow:auto}nav a{flex:1;text-align:center;white-space:nowrap}.estado{align-items:flex-start;flex-direction:column;gap:6px}.grade-destaque{grid-template-columns:1fr 1fr}.velocidade,.grade-destaque .percurso-card{grid-column:1/-1}.velocidade{min-height:160px}.grade-operacao>*{grid-column:1/-1}.metricas-link{grid-template-columns:1fr 1fr}.valor-gigante{font-size:4.4rem}footer{flex-direction:column}footer span:last-child{text-align:left}}"
    "@media(max-width:410px){.grade-destaque{grid-template-columns:1fr}.grade-destaque .percurso-card{grid-column:auto}.duplo{grid-template-columns:1fr}.simbolo{width:40px;height:40px}.metricas-link{grid-template-columns:1fr 1fr}}"
    "@media(prefers-reduced-motion:reduce){*{scroll-behavior:auto!important;transition:none!important}}"
    "</style></head><body><main><header class='topo'><div class='marca'><div class='simbolo'>E</div><div><div class='sobretitulo'>UFSM-CS · ESP32-S3</div><h1>Painel da equipe</h1></div></div>"
    "<nav aria-label='Navegação principal'><a class='ativo' href='/'>Equipe</a><a href='/diagnostico'>Diagnóstico</a><a href='/mestre'>Mestre</a></nav></header>"
    "<div id='estado' class='estado' aria-live='polite'><div class='estado-principal'><span class='ponto'></span><span id='estadoTexto'>Aguardando telemetria</span></div><small id='ultimaAtualizacao'>Conectando ao receptor UART...</small></div>"
    "<section class='grade-destaque' aria-label='Indicadores principais'><article class='cartao velocidade'><div><div class='rotulo'>Velocidade atual</div><div class='valor-gigante'><span id='velocidade'>—</span> <span class='unidade'>km/h</span></div></div><span id='fonte' class='fonte'>Fonte ainda não identificada</span></article>"
    "<article class='cartao'><div class='rotulo'>Bateria</div><div id='carga' class='valor'>—</div><div class='barra'><span id='cargaBarra'></span></div><div id='bateriaDetalhe' class='detalhe'>Aguardando tensão do pacote</div></article>"
    "<article class='cartao'><div class='rotulo'>Potência instantânea</div><div id='potencia' class='valor'>—</div><div class='linha-detalhes'><span><b id='tensao'>—</b> V</span><span><b id='corrente'>—</b> A</span></div><div class='detalhe'>Tensão e corrente do pacote</div></article>"
    "<article class='cartao percurso-card'><div class='rotulo'>Autonomia estimada</div><div id='autonomia' class='valor'>—</div><div class='linha-detalhes'><span>Percurso <b id='percurso'>—</b></span></div><div class='detalhe'>Distância acumulada nesta operação</div></article></section>"
    "<div class='titulo-secao'><div><div class='sobretitulo'>Veículo</div><h2>Operação e sensores</h2></div><p>Dados recebidos do mestre em tempo real</p></div>"
    "<section class='grade-operacao'><article class='cartao marcha'><div class='rotulo'>Marcha</div><div id='marcha' class='valor'>—</div><div class='detalhe'>Posição do seletor</div></article>"
    "<article class='cartao pedais'><div class='rotulo'>Comandos do piloto</div><div class='pedal'><span>Acelerador</span><div class='barra amarela'><span id='aceleradorBarra'></span></div><b id='acelerador'>—</b></div><div class='pedal'><span>Freio</span><div class='barra vermelha'><span id='freioBarra'></span></div><b id='freio'>—</b></div></article>"
    "<article class='cartao proximidade'><div class='rotulo'>Proximidade</div><div class='duplo'><div class='mini'><span class='rotulo'>Frente</span><strong id='frente'>—</strong></div><div class='mini'><span class='rotulo'>Traseira</span><strong id='traseira'>—</strong></div></div></article>"
    "<article class='cartao celulas'><div class='rotulo'>Tensão das células</div><div id='celulas' class='lista-celulas'><span class='celula'>Aguardando</span></div></article>"
    "<article class='cartao posicao'><div class='rotulo'>Posição GNSS</div><div class='coordenadas'><div id='latitude'>—</div><div id='longitude'>—</div></div><div class='detalhe'>Latitude e longitude em graus decimais</div></article>"
    "<article class='cartao comunicacao'><div class='rotulo'>Qualidade do enlace</div><div class='metricas-link'><div class='metrica-link'><span>Recepção</span><b id='taxa'>—</b></div><div class='metrica-link'><span>Perda</span><b id='perda'>—</b></div><div class='metrica-link'><span>Idade</span><b id='idade'>—</b></div><div class='metrica-link'><span>Sequência</span><b id='sequencia'>—</b></div><div class='metrica-link'><span>Válidos</span><b id='validos'>0</b></div><div class='metrica-link'><span>Inválidos</span><b id='invalidos'>0</b></div><div class='metrica-link'><span>Perdidos</span><b id='perdidos'>0</b></div><div class='metrica-link'><span>CRC-16</span><b id='crcs'>0</b></div></div><div class='linha-detalhes'><span>Intervalo / jitter <b id='jitter'>—</b></span><span>Atraso relativo <b id='latencia'>—</b></span></div></article></section>"
    "<footer><span>Atualização automática a cada 500 ms</span><span>Rede local do satélite da equipe · 192.168.4.1</span></footer></main><script>"
    "const $=id=>document.getElementById(id);let consultando=false;function texto(id,valor){$(id).textContent=valor}"
    "function percentual(valor){return Math.max(0,Math.min(100,Number(valor)||0))}function barra(id,valor){let e=$(id);e.style.width=percentual(valor)+'%';e.parentElement.setAttribute('aria-valuenow',percentual(valor))}"
    "function classeNumero(id,valor,limite){let e=$(id);e.classList.toggle('alerta',Number(valor)>limite);e.classList.toggle('bom',Number(valor)===0)}"
    "function mostrarCelulas(valores){let lista=$('celulas');lista.replaceChildren();(valores||[]).forEach((valor,indice)=>{let item=document.createElement('span');item.className='celula';item.textContent='C'+(indice+1)+'  '+(valor/1000).toFixed(3)+' V';lista.append(item)});if(!lista.children.length){let item=document.createElement('span');item.className='celula';item.textContent='Sem leituras';lista.append(item)}}"
    "function aplicar(d){let marchas=['P','N','D','R'],carga=d.carga_decimos_percentual/10,acelerador=d.acelerador_decimos_percentual/10,freio=d.freio_decimos_percentual/10;"
    "texto('velocidade',(d.velocidade_centesimos_kmh/100).toFixed(1));texto('marcha',marchas[d.marcha]||'?');texto('carga',carga.toFixed(1)+'%');barra('cargaBarra',carga);texto('bateriaDetalhe',(d.tensao_pacote_mv/1000).toFixed(2)+' V disponíveis no pacote');"
    "texto('potencia',d.potencia_w+' W');texto('tensao',(d.tensao_pacote_mv/1000).toFixed(2));texto('corrente',(d.corrente_centesimos_a/100).toFixed(2));texto('autonomia',(d.autonomia_decimos_km/10).toFixed(1)+' km');texto('percurso',(d.percurso_metros/1000).toFixed(3)+' km');"
    "texto('acelerador',acelerador.toFixed(1)+'%');texto('freio',freio.toFixed(1)+'%');barra('aceleradorBarra',acelerador);barra('freioBarra',freio);texto('frente',d.distancias_cm[0]+' cm');texto('traseira',d.distancias_cm[4]+' cm');mostrarCelulas(d.tensoes_celulas_mv);"
    "texto('latitude',(d.latitude_micrograus/1e6).toFixed(6)+'°');texto('longitude',(d.longitude_micrograus/1e6).toFixed(6)+'°');texto('sequencia','#'+d.sequencia);texto('validos',d.pacotes_validos);texto('invalidos',d.pacotes_invalidos);texto('perdidos',d.pacotes_perdidos);texto('crcs',d.falhas_crc);"
    "texto('taxa',(d.taxa_pacotes_centesimos_hz/100).toFixed(2)+' pkt/s');texto('jitter',d.intervalo_medio_ms+' / '+d.jitter_medio_ms+' ms');texto('latencia',d.latencia_relativa_ms+' ms');texto('perda',(d.taxa_perda_centesimos_percentual/100).toFixed(2)+'%');texto('idade',d.idade_ms+' ms');classeNumero('invalidos',d.pacotes_invalidos,0);classeNumero('perdidos',d.pacotes_perdidos,0);classeNumero('crcs',d.falhas_crc,0);"
    "let recente=d.valido&&d.idade_ms<2000,estado=$('estado');estado.className='estado '+(recente?(d.simulado?'simulado':'online'):'offline');texto('estadoTexto',recente?(d.simulado?'Telemetria simulada ativa':'Telemetria real recebida'):'Sem telemetria recente');texto('fonte',d.simulado?'Dados gerados pelo simulador':'Sensores reais do veículo');texto('ultimaAtualizacao',recente?'Amostra com '+d.idade_ms+' ms · atualizada às '+new Date().toLocaleTimeString():'Última amostra com '+d.idade_ms+' ms');document.body.classList.toggle('offline',!recente)}"
    "async function atualizar(){if(consultando)return;consultando=true;try{let resposta=await fetch('/api/telemetria',{cache:'no-store'});if(!resposta.ok)throw new Error('HTTP '+resposta.status);aplicar(await resposta.json())}catch(erro){let estado=$('estado');estado.className='estado offline';texto('estadoTexto','Painel sem comunicação');texto('ultimaAtualizacao','Não foi possível consultar a API local');document.body.classList.add('offline')}finally{consultando=false}}"
    "atualizar();setInterval(atualizar,500);</script></body></html>";

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

static esp_err_t responder_painel(httpd_req_t *requisicao)
{
    httpd_resp_set_hdr(requisicao, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_type(requisicao, "text/html; charset=utf-8");
    return httpd_resp_send(requisicao, pagina_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t responder_mestre_pagina(httpd_req_t *requisicao)
{
    httpd_resp_set_hdr(requisicao, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_type(requisicao, "text/html; charset=utf-8");
    return httpd_resp_send(requisicao, PAGINA_MESTRE_WEB,
                           HTTPD_RESP_USE_STRLEN);
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
    if (strcmp(consulta, "capacidades") == 0) return COMANDO_OBTER_CAPACIDADES;
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

static void codificar_hexadecimal(char *destino, size_t capacidade,
                                  const uint8_t *dados, size_t tamanho)
{
    static const char DIGITOS[] = "0123456789ABCDEF";
    if (destino == NULL || capacidade == 0) return;
    size_t saida = 0;
    for (size_t indice = 0; indice < tamanho && saida + 2 < capacidade; ++indice) {
        destino[saida++] = DIGITOS[dados[indice] >> 4];
        destino[saida++] = DIGITOS[dados[indice] & 0x0Fu];
    }
    destino[saida] = '\0';
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
    informacoes_solicitacao_comando_t informacoes = {0};
    const esp_err_t erro = servico_comandos_satelite_solicitar_detalhado(
        comando, NULL, 0, resposta, sizeof(resposta), &tamanho_resposta,
        &resultado, pdMS_TO_TICKS(1500), &informacoes);
    if (erro != ESP_OK) {
        if (acao) {
            ESP_LOGW(TAG, "Mestre não confirmou a ação '%s': %s",
                     consulta, esp_err_to_name(erro));
        }
        char json[384];
        snprintf(json, sizeof(json),
                 "{\"ok\":false,\"consulta\":\"%s\",\"comando\":%u,"
                 "\"solicitacao\":%lu,\"tentativas\":%u,\"duracao_ms\":%lu,"
                 "\"erro\":\"%s\",\"codigo_esp\":%ld}",
                 consulta, (unsigned)comando,
                 (unsigned long)informacoes.identificador,
                 (unsigned)informacoes.tentativas,
                 (unsigned long)informacoes.duracao_ms,
                 esp_err_to_name(erro), (long)erro);
        return enviar_json_comando(
            requisicao, erro == ESP_ERR_TIMEOUT
                             ? "504 Gateway Timeout" : "502 Bad Gateway",
            json);
    }
    if (resultado != RESPOSTA_COMANDO_OK) {
        if (acao) {
            ESP_LOGW(TAG, "Mestre recusou a ação '%s' com resultado %u",
                     consulta, (unsigned)resultado);
        }
        char json[384];
        snprintf(json, sizeof(json),
                 "{\"ok\":false,\"consulta\":\"%s\",\"comando\":%u,"
                 "\"solicitacao\":%lu,\"resultado\":%u,\"tentativas\":%u,"
                 "\"duracao_ms\":%lu,\"erro\":\"%.63s\"}",
                 consulta, (unsigned)comando,
                 (unsigned long)informacoes.identificador, (unsigned)resultado,
                 (unsigned)informacoes.tentativas,
                 (unsigned long)informacoes.duracao_ms,
                 descrever_resultado_comando(resultado));
        return enviar_json_comando(requisicao, "409 Conflict", json);
    }

    char json[1536];
    int tamanho = -1;
    if (comando == COMANDO_PING && tamanho_resposta == sizeof(resposta_comando_ping_t)) {
        resposta_comando_ping_t dados;
        memcpy(&dados, resposta, sizeof(dados));
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"ping\",\"comando\":%u,\"solicitacao\":%lu,"
            "\"resultado\":0,\"tentativas\":%u,\"duracao_ms\":%lu,"
            "\"rtt_ms\":%lu,\"tempo_ativo_ms\":%lu}",
            (unsigned)comando, (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms,
            (unsigned long)informacoes.duracao_ms,
            (unsigned long)dados.tempo_ativo_ms);
    } else if (comando == COMANDO_OBTER_VERSAO &&
               tamanho_resposta == sizeof(resposta_comando_versao_t)) {
        resposta_comando_versao_t dados;
        char versao[65];
        memcpy(&dados, resposta, sizeof(dados));
        dados.versao[sizeof(dados.versao) - 1] = '\0';
        escapar_texto_json(versao, sizeof(versao), dados.versao);
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"versao\",\"comando\":%u,\"solicitacao\":%lu,"
            "\"resultado\":0,\"tentativas\":%u,\"duracao_ms\":%lu,"
            "\"firmware\":\"%s\",\"capacidades\":%lu,\"no\":%u}",
            (unsigned)comando, (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms, versao,
            (unsigned long)dados.capacidades, (unsigned)dados.no);
    } else if (comando == COMANDO_OBTER_CAPACIDADES &&
               tamanho_resposta == sizeof(resposta_comando_capacidades_t)) {
        resposta_comando_capacidades_t dados;
        memcpy(&dados, resposta, sizeof(dados));
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"capacidades\",\"comando\":%u,\"solicitacao\":%lu,"
            "\"resultado\":0,\"tentativas\":%u,\"duracao_ms\":%lu,\"capacidades\":%lu}",
            (unsigned)comando, (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms,
            (unsigned long)dados.capacidades);
    } else if (comando == COMANDO_OBTER_SAUDE &&
               tamanho_resposta == sizeof(resposta_comando_saude_t)) {
        resposta_comando_saude_t dados;
        memcpy(&dados, resposta, sizeof(dados));
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"saude\",\"comando\":%u,\"solicitacao\":%lu,"
            "\"resultado\":0,\"tentativas\":%u,\"duracao_ms\":%lu,\"estado\":%u,"
            "\"causas_ativas\":%lu,\"tempo_ativo_ms\":%lu,\"heap_livre\":%lu,\"erros_historicos\":%lu}",
            (unsigned)comando, (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms, (unsigned)dados.estado,
            (unsigned long)dados.causas_ativas,
            (unsigned long)dados.tempo_ativo_ms, (unsigned long)dados.heap_livre,
            (unsigned long)dados.erros_historicos);
    } else if (comando == COMANDO_OBTER_TELEMETRIA &&
               tamanho_resposta == sizeof(resposta_comando_telemetria_t)) {
        resposta_comando_telemetria_t dados;
        memcpy(&dados, resposta, sizeof(dados));
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"telemetria\",\"comando\":%u,\"solicitacao\":%lu,"
            "\"resultado\":0,\"tentativas\":%u,\"duracao_ms\":%lu,\"sequencia\":%u,"
            "\"tempo_mestre_ms\":%lu,\"adc\":%u,\"velocidade_kmh\":%.2f,\"carga_percentual\":%.1f}",
            (unsigned)comando, (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms, (unsigned)dados.sequencia,
            (unsigned long)dados.tempo_mestre_ms,
            (unsigned)dados.tensao_adc_bruta,
            dados.velocidade_centesimos_kmh / 100.0,
            dados.carga_decimos_percentual / 10.0);
    } else if (comando == COMANDO_OBTER_ESTADO_UART &&
               tamanho_resposta == sizeof(resposta_comando_uart_t)) {
        resposta_comando_uart_t dados;
        memcpy(&dados, resposta, sizeof(dados));
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"uart\",\"comando\":%u,\"solicitacao\":%lu,"
            "\"resultado\":0,\"tentativas\":%u,\"duracao_ms\":%lu,\"envios\":%lu,"
            "\"recepcoes\":%lu,\"erros\":%lu,\"descartes\":%lu}",
            (unsigned)comando, (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms,
            (unsigned long)dados.envios, (unsigned long)dados.recepcoes,
            (unsigned long)dados.erros, (unsigned long)dados.descartes);
    } else if (comando == COMANDO_OBTER_ESTADO_OTA &&
               tamanho_resposta == sizeof(resposta_comando_ota_detalhada_t)) {
        resposta_comando_ota_detalhada_t dados;
        char mestre_atual[37], mestre_disponivel[37];
        char equipe_atual[37], equipe_disponivel[37];
        char visitantes_atual[37], visitantes_disponivel[37];
        memcpy(&dados, resposta, sizeof(dados));
        if (dados.versao_formato != COMANDO_FORMATO_OTA_DETALHADO) {
            ESP_LOGW(TAG, "Resposta OTA detalhada com formato desconhecido: %u",
                     (unsigned)dados.versao_formato);
            return enviar_json_comando(
                requisicao, "502 Bad Gateway",
                "{\"ok\":false,\"erro\":\"formato da resposta OTA não suportado\"}");
        }
        dados.versao_mestre_atual[sizeof(dados.versao_mestre_atual) - 1] = '\0';
        dados.versao_mestre_disponivel[sizeof(dados.versao_mestre_disponivel) - 1] = '\0';
        dados.versao_equipe_atual[sizeof(dados.versao_equipe_atual) - 1] = '\0';
        dados.versao_equipe_disponivel[sizeof(dados.versao_equipe_disponivel) - 1] = '\0';
        dados.versao_visitantes_atual[sizeof(dados.versao_visitantes_atual) - 1] = '\0';
        dados.versao_visitantes_disponivel[sizeof(dados.versao_visitantes_disponivel) - 1] = '\0';
        escapar_texto_json(mestre_atual, sizeof(mestre_atual), dados.versao_mestre_atual);
        escapar_texto_json(mestre_disponivel, sizeof(mestre_disponivel), dados.versao_mestre_disponivel);
        escapar_texto_json(equipe_atual, sizeof(equipe_atual), dados.versao_equipe_atual);
        escapar_texto_json(equipe_disponivel, sizeof(equipe_disponivel), dados.versao_equipe_disponivel);
        escapar_texto_json(visitantes_atual, sizeof(visitantes_atual), dados.versao_visitantes_atual);
        escapar_texto_json(visitantes_disponivel, sizeof(visitantes_disponivel), dados.versao_visitantes_disponivel);
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"ota\",\"comando\":%u,\"solicitacao\":%lu,"
            "\"resultado\":0,\"tentativas\":%u,\"duracao_ms\":%lu,\"formato\":%u,"
            "\"estado\":%u,\"alvo\":%u,\"falhas\":%lu,\"verificacoes\":%lu,"
            "\"mestre\":{\"estado\":%u,\"progresso_percentual\":%.1f,\"versao_atual\":\"%s\",\"versao_disponivel\":\"%s\"},"
            "\"equipe\":{\"estado\":%u,\"progresso_percentual\":%.1f,\"versao_atual\":\"%s\",\"versao_disponivel\":\"%s\"},"
            "\"visitantes\":{\"estado\":%u,\"progresso_percentual\":%.1f,\"versao_atual\":\"%s\",\"versao_disponivel\":\"%s\"}}",
            (unsigned)comando, (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms,
            (unsigned)dados.versao_formato, (unsigned)dados.estado,
            (unsigned)dados.alvo_ativo, (unsigned long)dados.falhas,
            (unsigned long)dados.verificacoes, (unsigned)dados.estado_mestre,
            dados.progresso_mestre_decimos / 10.0, mestre_atual, mestre_disponivel,
            (unsigned)dados.estado_equipe,
            dados.progresso_equipe_decimos / 10.0, equipe_atual, equipe_disponivel,
            (unsigned)dados.estado_visitantes,
            dados.progresso_visitantes_decimos / 10.0,
            visitantes_atual, visitantes_disponivel);
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
            "{\"ok\":true,\"consulta\":\"ota\",\"comando\":%u,\"solicitacao\":%lu,"
            "\"resultado\":0,\"tentativas\":%u,\"duracao_ms\":%lu,\"formato\":1,"
            "\"estado\":%u,\"alvo\":%u,\"progresso_percentual\":%.1f,\"falhas\":%lu,"
            "\"versao_atual\":\"%s\",\"versao_disponivel\":\"%s\"}",
            (unsigned)comando, (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms,
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
            "{\"ok\":true,\"consulta\":\"wifi\",\"comando\":%u,\"solicitacao\":%lu,"
            "\"resultado\":0,\"tentativas\":%u,\"duracao_ms\":%lu,"
            "\"radio_ativo\":%s,\"conectado\":%s,\"rssi_dbm\":%d,\"canal\":%u,\"rede\":\"%s\"}",
            (unsigned)comando, (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms,
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
            "{\"ok\":true,\"consulta\":\"%s\",\"acao\":\"%s\",\"comando\":%u,"
            "\"solicitacao\":%lu,\"resultado\":0,\"tentativas\":%u,\"duracao_ms\":%lu,"
            "\"confirmada\":true,\"tempo_mestre_ms\":%lu,"
            "\"mensagem\":\"solicitação aceita pelo mestre\"}",
            consulta, consulta, (unsigned)comando,
            (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms,
            (unsigned long)dados.tempo_mestre_ms);
    } else if (acao && tamanho_resposta == 0) {
        // Compatibilidade durante a atualização gradual: a primeira versão
        // dos comandos confirmava a ação somente pelo código do cabeçalho.
        ESP_LOGW(TAG, "Mestre confirmou a ação '%s' no formato legado",
                 consulta);
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"%s\",\"acao\":\"%s\",\"comando\":%u,"
            "\"solicitacao\":%lu,\"resultado\":0,\"tentativas\":%u,\"duracao_ms\":%lu,"
            "\"confirmada\":true,\"formato\":\"legado\","
            "\"mensagem\":\"solicitação aceita pelo mestre\"}",
            consulta, consulta, (unsigned)comando,
            (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms);
    } else if (!acao) {
        char carga_hex[COMANDO_TAMANHO_MAXIMO_CARGA * 2 + 1];
        codificar_hexadecimal(carga_hex, sizeof(carga_hex), resposta,
                              tamanho_resposta);
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"%s\",\"comando\":%u,\"solicitacao\":%lu,"
            "\"resultado\":0,\"tentativas\":%u,\"duracao_ms\":%lu,"
            "\"formato\":\"binario\",\"tamanho_resposta\":%u,\"carga_hex\":\"%s\"}",
            consulta, (unsigned)comando,
            (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms,
            (unsigned)tamanho_resposta, carga_hex);
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
    // Os manipuladores montam respostas de diagnóstico e OTA na pilha.
    configuracao.stack_size = 8192;
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
