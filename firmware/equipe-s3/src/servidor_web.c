// Implementa as rotas HTTP e publica a telemetria recebida.
#include "servidor_web.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "configuracao.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
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
#include "servico_ota_satelite.h"

static const char *TAG = "servidor_web";

static const char pagina_html[] =
    "<!doctype html><html lang='pt-BR'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta name='theme-color' content='#050b12'><title>Painel da equipe | UFSM Carro</title><style>"
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
    ".titulo-secao{display:flex;justify-content:space-between;align-items:end;gap:12px;margin:0 0 11px}.titulo-secao h2{font-size:1rem;margin:0}.titulo-secao p{color:var(--fraco);font-size:.8rem;margin:0}.grade-operacao{display:grid;grid-template-columns:repeat(12,1fr);gap:12px;margin-bottom:24px}.marcha{grid-column:span 2}.pedais{grid-column:span 4}.proximidade{grid-column:span 3}.celulas{grid-column:span 3}.mpu{grid-column:1/-1}.grade-mpu{display:grid;grid-template-columns:repeat(6,1fr);gap:9px;margin-top:13px}.posicao{grid-column:span 5}.comunicacao{grid-column:span 7}"
    ".marcha .valor{font-size:3.4rem;color:var(--amarelo)}.pedal{display:grid;grid-template-columns:76px 1fr 48px;align-items:center;gap:9px;margin-top:13px;font-size:.8rem}.pedal .barra{margin:0}.pedal b{text-align:right}.duplo{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:15px}.mini{background:#0c1721;border:1px solid #203343;border-radius:10px;padding:11px}.mini strong{display:block;font-size:1.2rem;margin-top:5px}.lista-celulas{display:flex;flex-wrap:wrap;gap:7px;margin-top:13px}.celula{background:#0b1620;border:1px solid #263d50;color:#c6d4df;border-radius:7px;padding:6px 8px;font:650 .76rem ui-monospace,SFMono-Regular,monospace}"
    ".coordenadas{font:650 1.08rem ui-monospace,SFMono-Regular,monospace;line-height:1.55;margin-top:10px}.metricas-link{display:grid;grid-template-columns:repeat(4,1fr);gap:1px;background:var(--borda);border:1px solid var(--borda);border-radius:11px;overflow:hidden;margin-top:13px}.metrica-link{background:#0c1721;padding:11px}.metrica-link span{display:block;color:var(--fraco);font-size:.68rem;text-transform:uppercase;letter-spacing:.07em}.metrica-link b{display:block;margin-top:5px;font-size:1rem}.alerta{color:var(--vermelho)!important}.bom{color:var(--verde)!important}"
    "footer{display:flex;justify-content:space-between;gap:12px;color:var(--fraco);font-size:.78rem;padding-top:2px}footer span:last-child{text-align:right}"
    ".offline .cartao{filter:saturate(.72)}@media(max-width:980px){.grade-destaque{grid-template-columns:1.25fr 1fr 1fr}.grade-destaque .percurso-card{grid-column:1/-1}.marcha{grid-column:span 3}.pedais{grid-column:span 5}.proximidade{grid-column:span 4}.celulas{grid-column:span 5}.grade-mpu{grid-template-columns:repeat(3,1fr)}.posicao{grid-column:span 7}.comunicacao{grid-column:1/-1}}"
    "@media(max-width:680px){main{padding:16px 14px 28px}.topo{align-items:flex-start;flex-direction:column}nav{width:100%;overflow:auto}nav a{flex:1;text-align:center;white-space:nowrap}.estado{align-items:flex-start;flex-direction:column;gap:6px}.grade-destaque{grid-template-columns:1fr 1fr}.velocidade,.grade-destaque .percurso-card{grid-column:1/-1}.velocidade{min-height:160px}.grade-operacao>*{grid-column:1/-1}.grade-mpu{grid-template-columns:1fr 1fr}.metricas-link{grid-template-columns:1fr 1fr}.valor-gigante{font-size:4.4rem}footer{flex-direction:column}footer span:last-child{text-align:left}}"
    "@media(max-width:410px){.grade-destaque{grid-template-columns:1fr}.grade-destaque .percurso-card{grid-column:auto}.duplo{grid-template-columns:1fr}.simbolo{width:40px;height:40px}.metricas-link{grid-template-columns:1fr 1fr}}"
    "[hidden]{display:none!important}:root{--fundo:#050b12;--superficie:#0b1722;--elevado:#102130;--borda:#20384b;--texto:#f7fbff;--fraco:#94aabd;--amarelo:#ffca46;--verde:#46dfa0;--vermelho:#ff687c;--azul:#4aaef1;--ciano:#36d2e2}"
    "body{background:radial-gradient(circle at 88% -8%,#123d59 0,transparent 34rem),radial-gradient(circle at -10% 55%,#13332f 0,transparent 30rem),var(--fundo);font-feature-settings:'tnum' 1}body:before{opacity:.12;background-size:48px 48px}body:after{content:'';position:fixed;inset:0;pointer-events:none;background:linear-gradient(115deg,transparent 0 46%,#35d5e504 50%,transparent 54%);z-index:-1}"
    "main{max-width:1320px;padding:25px 28px 38px}.topo{margin-bottom:20px}.simbolo{width:52px;height:52px;border-radius:17px;background:linear-gradient(145deg,#ffe080,#e5a329);font-size:.78rem;letter-spacing:.04em;box-shadow:0 12px 34px #f5b94232,0 0 0 1px #ffe08a66 inset}.marca h1{font-size:1.48rem;letter-spacing:-.025em}.topo-direita{display:flex;align-items:center;gap:12px}.selo-live{display:flex;align-items:center;gap:7px;color:#a9f7d2;border:1px solid #2c6850;background:#0b211bbd;border-radius:99px;padding:9px 12px;font-size:.7rem;font-weight:800;letter-spacing:.08em;white-space:nowrap}.selo-live i{width:7px;height:7px;border-radius:50%;background:var(--verde);box-shadow:0 0 14px var(--verde);animation:pulsar 1.8s ease-in-out infinite}"
    "nav{border-radius:99px;padding:4px;box-shadow:0 12px 30px #0003}nav a{border-radius:99px;padding:9px 15px}.estado{min-height:51px;border-radius:17px;background:linear-gradient(110deg,#0c1a26e8,#0a151fe8);box-shadow:0 16px 40px #0003}.estado small{white-space:normal}.estado-info{display:flex;align-items:center;gap:12px}.botao-atualizar{border:1px solid #2b465b;background:#112433;color:var(--texto);border-radius:10px;padding:8px 11px;font:700 .75rem system-ui;cursor:pointer}.botao-atualizar:hover{border-color:var(--ciano);background:#153044}.botao-atualizar:focus-visible{outline:2px solid var(--amarelo);outline-offset:2px}"
    ".grade-destaque{grid-template-columns:1.7fr repeat(3,minmax(0,1fr));gap:14px}.cartao{border-radius:20px;padding:19px;background:linear-gradient(145deg,#112432,#0a151f);box-shadow:0 16px 40px #0003;transition:transform .2s ease,border-color .2s ease,box-shadow .2s ease}.cartao:before{content:'';position:absolute;left:18px;right:18px;top:0;height:2px;border-radius:0 0 3px 3px;background:linear-gradient(90deg,var(--brilho,var(--azul)),transparent);opacity:.7}.cartao:hover{transform:translateY(-2px);border-color:#34556c;box-shadow:0 20px 46px #0004}.velocidade{min-height:248px;--brilho:var(--amarelo);background:radial-gradient(circle at 85% 15%,#5e4a1729,transparent 45%),linear-gradient(145deg,#18251f,#0b1722 70%)}.valor-gigante{font-size:clamp(4.1rem,7.6vw,6.8rem);font-weight:800}.grafico-velocidade{display:block;width:100%;height:75px;margin:8px 0 7px}.fonte{position:relative;z-index:1}.bateria-card{--brilho:var(--verde)}.energia-card{--brilho:var(--ciano)}.percurso-card{--brilho:var(--azul)}"
    ".anel-bateria{--nivel-carga:0deg;--cor-carga:var(--verde);position:relative;display:grid;place-items:center;width:112px;height:112px;margin:15px auto 10px;border-radius:50%;background:conic-gradient(var(--cor-carga) var(--nivel-carga),#07121b var(--nivel-carga));box-shadow:0 0 28px color-mix(in srgb,var(--cor-carga) 18%,transparent)}.anel-bateria:before{content:'';position:absolute;inset:10px;border-radius:50%;background:linear-gradient(145deg,#122737,#09141e);box-shadow:inset 0 0 0 1px #ffffff0b}.anel-bateria .valor{position:relative;margin:0;font-size:1.42rem}.bateria-card .detalhe{text-align:center}.bateria-card>.barra{display:none}"
    ".titulo-secao{margin-top:2px}.titulo-secao h2{font-size:1.12rem}.grade-operacao{gap:14px}.marcha{--brilho:var(--amarelo)}.pedais{--brilho:#ff9f50}.proximidade{--brilho:var(--verde)}.celulas{--brilho:#ba8cff}.mpu{--brilho:var(--ciano)}.posicao{--brilho:var(--azul)}.comunicacao{--brilho:var(--verde)}.mini{border-radius:13px;background:#091721;border-color:#1e3a4d}.celula{border-radius:9px;background:#08151f}.metricas-link{border-radius:14px}.metrica-link{background:#091721;min-width:0}.metrica-link b{overflow-wrap:anywhere}"
    "footer{border-top:1px solid #173044;padding-top:16px}.rodape-frequencia{display:inline-flex;align-items:center;gap:7px}.rodape-frequencia:before{content:'';width:6px;height:6px;border-radius:50%;background:var(--ciano);box-shadow:0 0 10px var(--ciano)}@keyframes pulsar{50%{opacity:.45;transform:scale(.75)}}"
    "@media(max-width:1080px){.grade-destaque{grid-template-columns:1.35fr 1fr 1fr}.grade-destaque .percurso-card{grid-column:1/-1}}@media(max-width:760px){main{padding:17px 14px 29px}.topo,.topo-direita{width:100%;align-items:flex-start;flex-direction:column}.topo-direita{gap:9px}.selo-live{position:absolute;right:14px;top:19px}nav{width:100%;overflow:auto}.estado{align-items:flex-start}.estado-info{width:100%;justify-content:space-between}.grade-destaque{grid-template-columns:1fr 1fr}.velocidade,.grade-destaque .percurso-card{grid-column:1/-1}.velocidade{min-height:225px}.metricas-link{grid-template-columns:1fr 1fr}}@media(max-width:430px){.selo-live{font-size:.62rem;padding:7px 9px}.marca h1{font-size:1.25rem}.valor-gigante{font-size:4.7rem}.grade-destaque{grid-template-columns:1fr}.grade-destaque .percurso-card{grid-column:auto}.anel-bateria{margin-left:0}.bateria-card .detalhe{text-align:left}}"
    "@media(prefers-reduced-motion:reduce){*{scroll-behavior:auto!important;transition:none!important}}"
    "</style></head><body><main><header class='topo'><div class='marca'><div class='simbolo'>CS</div><div><div class='sobretitulo'>UFSM-CS · ESP32-S3</div><h1>Painel da equipe</h1></div></div>"
    "<div class='topo-direita'><span class='selo-live'><i></i>AO VIVO · 4 Hz</span><nav aria-label='Navegação principal'><a class='ativo' href='http://painel.equipe/'>Equipe</a><a href='http://painel.diagnostico/'>Diagnóstico</a><a href='http://painel.mestre/'>Mestre</a></nav></div></header>"
    "<div id='estado' class='estado' aria-live='polite'><div class='estado-principal'><span class='ponto'></span><span id='estadoTexto'>Aguardando telemetria</span></div><div class='estado-info'><small id='ultimaAtualizacao'>Conectando ao receptor UART...</small><button id='atualizarAgora' class='botao-atualizar' type='button'>Atualizar agora</button></div></div>"
    "<section class='grade-destaque' aria-label='Indicadores principais'><article class='cartao velocidade'><div><div class='rotulo'>Velocidade atual</div><div class='valor-gigante'><span id='velocidade'>—</span> <span class='unidade'>km/h</span></div></div><canvas id='graficoVelocidade' class='grafico-velocidade' aria-label='Histórico recente da velocidade'></canvas><span id='fonte' class='fonte'>Fonte ainda não identificada</span></article>"
    "<article class='cartao bateria-card'><div class='rotulo'>Bateria</div><div id='medidorBateria' class='anel-bateria'><div id='carga' class='valor'>—</div></div><div class='barra'><span id='cargaBarra'></span></div><div id='bateriaDetalhe' class='detalhe'>Aguardando tensão do pacote</div></article>"
    "<article class='cartao energia-card'><div class='rotulo'>Potência instantânea</div><div id='potencia' class='valor'>—</div><div class='linha-detalhes'><span><b id='tensao'>—</b> V</span><span><b id='corrente'>—</b> A</span></div><div class='detalhe'>Tensão e corrente do pacote</div></article>"
    "<article class='cartao percurso-card'><div class='rotulo'>Autonomia estimada</div><div id='autonomia' class='valor'>—</div><div class='linha-detalhes'><span>Percurso <b id='percurso'>—</b></span></div><div class='detalhe'>Distância acumulada nesta operação</div></article></section>"
    "<div class='titulo-secao'><div><div class='sobretitulo'>Veículo</div><h2>Operação e sensores</h2></div><p>Dados recebidos do mestre em tempo real</p></div>"
    "<section class='grade-operacao'><article class='cartao marcha'><div class='rotulo'>Marcha</div><div id='marcha' class='valor'>—</div><div class='detalhe'>Posição do seletor</div></article>"
    "<article class='cartao pedais'><div class='rotulo'>Comandos do piloto</div><div class='pedal'><span>Acelerador</span><div class='barra amarela'><span id='aceleradorBarra'></span></div><b id='acelerador'>—</b></div><div class='pedal'><span>Freio</span><div class='barra vermelha'><span id='freioBarra'></span></div><b id='freio'>—</b></div></article>"
    "<article class='cartao proximidade'><div class='rotulo'>Proximidade</div><div class='duplo'><div class='mini'><span class='rotulo'>Frente</span><strong id='frente'>—</strong></div><div class='mini'><span class='rotulo'>Traseira</span><strong id='traseira'>—</strong></div></div></article>"
    "<article class='cartao celulas'><div class='rotulo'>Tensão das células</div><div id='celulas' class='lista-celulas'><span class='celula'>Aguardando</span></div></article>"
    "<article class='cartao mpu'><div class='rotulo'>MPU6050 · dinâmica longitudinal</div><div class='grade-mpu'><div class='mini'><span class='rotulo'>Longitudinal</span><strong id='mpuLongitudinal'>—</strong></div><div class='mini'><span class='rotulo'>Sentido do vetor</span><strong id='mpuSentido'>—</strong></div><div class='mini'><span class='rotulo'>Aceleração X / Y / Z</span><strong id='mpuAceleracao'>—</strong></div><div class='mini'><span class='rotulo'>Giro X / Y / Z</span><strong id='mpuGiro'>—</strong></div><div class='mini'><span class='rotulo'>Temperatura</span><strong id='mpuTemperatura'>—</strong></div><div class='mini'><span class='rotulo'>Aquisição</span><strong id='mpuEstado'>—</strong></div></div><div class='detalhe'>Frente/trás indica o sentido da aceleração no eixo configurado, não o sentido de deslocamento.</div></article>"
    "<article class='cartao posicao'><div class='rotulo'>Posição GNSS</div><div class='coordenadas'><div id='latitude'>—</div><div id='longitude'>—</div></div><div class='linha-detalhes'><span>Rumo <b id='rumo'>—</b></span><span>Odômetro <b id='odometro'>—</b></span></div><div class='detalhe'>Latitude e longitude em graus decimais</div></article>"
    "<article class='cartao comunicacao'><div class='rotulo'>Qualidade do enlace</div><div class='metricas-link'><div class='metrica-link'><span>Recepção</span><b id='taxa'>—</b></div><div class='metrica-link'><span>Perda</span><b id='perda'>—</b></div><div class='metrica-link'><span>Idade</span><b id='idade'>—</b></div><div class='metrica-link'><span>Sequência</span><b id='sequencia'>—</b></div><div class='metrica-link'><span>Válidos</span><b id='validos'>0</b></div><div class='metrica-link'><span>Inválidos</span><b id='invalidos'>0</b></div><div class='metrica-link'><span>Perdidos</span><b id='perdidos'>0</b></div><div class='metrica-link'><span>Duplicados</span><b id='duplicados'>0</b></div><div class='metrica-link'><span>CRC-32</span><b id='crcs'>0</b></div><div class='metrica-link'><span>Cabeçalho</span><b id='cabecalhos'>0</b></div><div class='metrica-link'><span>Conteúdo</span><b id='conteudos'>0</b></div><div class='metrica-link'><span>Reinícios mestre</span><b id='reinicios'>0</b></div></div><div class='linha-detalhes'><span>Intervalo / jitter <b id='jitter'>—</b></span><span>Atraso relativo <b id='latencia'>—</b></span></div></article></section>"
    "<footer><span class='rodape-frequencia'>Atualização automática a 4 Hz</span><span>Acesso local · painel.equipe</span></footer></main><script>"
    "const $=id=>document.getElementById(id);let consultando=false,historicoVelocidade=[];function texto(id,valor){$(id).textContent=valor}"
    "function percentual(valor){return Math.max(0,Math.min(100,Number(valor)||0))}function barra(id,valor){let e=$(id);e.style.width=percentual(valor)+'%';e.parentElement.setAttribute('aria-valuenow',percentual(valor))}"
    "function classeNumero(id,valor,limite){let e=$(id);e.classList.toggle('alerta',Number(valor)>limite);e.classList.toggle('bom',Number(valor)===0)}"
    "function desenharGraficoVelocidade(valor){let tela=$('graficoVelocidade'),caixa=tela.getBoundingClientRect();if(!caixa.width)return;let escala=Math.min(window.devicePixelRatio||1,2),largura=Math.round(caixa.width*escala),altura=Math.round(caixa.height*escala);if(tela.width!==largura||tela.height!==altura){tela.width=largura;tela.height=altura}let ctx=tela.getContext('2d'),w=caixa.width,h=caixa.height;ctx.setTransform(escala,0,0,escala,0,0);ctx.clearRect(0,0,w,h);historicoVelocidade.push(Number(valor)||0);if(historicoVelocidade.length>64)historicoVelocidade.shift();let maximo=Math.max(30,...historicoVelocidade)*1.12,passo=w/Math.max(1,historicoVelocidade.length-1);ctx.strokeStyle='#8bb6cf18';ctx.lineWidth=1;for(let y=12;y<h;y+=22){ctx.beginPath();ctx.moveTo(0,y);ctx.lineTo(w,y);ctx.stroke()}let pontos=historicoVelocidade.map((v,i)=>[i*passo,h-7-(v/maximo)*(h-14)]);if(!pontos.length)return;ctx.beginPath();pontos.forEach((p,i)=>i?ctx.lineTo(p[0],p[1]):ctx.moveTo(p[0],p[1]));ctx.lineTo(pontos[pontos.length-1][0],h);ctx.lineTo(0,h);ctx.closePath();let gradiente=ctx.createLinearGradient(0,0,0,h);gradiente.addColorStop(0,'#36d2e238');gradiente.addColorStop(1,'#36d2e200');ctx.fillStyle=gradiente;ctx.fill();ctx.beginPath();pontos.forEach((p,i)=>i?ctx.lineTo(p[0],p[1]):ctx.moveTo(p[0],p[1]));ctx.strokeStyle='#47dce9';ctx.lineWidth=2;ctx.lineJoin='round';ctx.stroke();let ultimo=pontos[pontos.length-1];ctx.beginPath();ctx.arc(ultimo[0],ultimo[1],3.5,0,Math.PI*2);ctx.fillStyle='#ffca46';ctx.fill()}"
    "function mostrarCelulas(valores){let lista=$('celulas');lista.replaceChildren();(valores||[]).forEach((valor,indice)=>{let item=document.createElement('span');item.className='celula';item.textContent='C'+(indice+1)+'  '+(valor/1000).toFixed(3)+' V';lista.append(item)});if(!lista.children.length){let item=document.createElement('span');item.className='celula';item.textContent='Sem leituras';lista.append(item)}}"
    "function aplicar(d){let marchas=['P','N','D','R'],carga=d.carga_decimos_percentual/10,acelerador=d.acelerador_decimos_percentual/10,freio=d.freio_decimos_percentual/10;"
    "let velocidade=d.velocidade_centesimos_kmh/100;texto('velocidade',velocidade.toFixed(1));desenharGraficoVelocidade(velocidade);texto('marcha',marchas[d.marcha]||'?');texto('carga',carga.toFixed(1)+'%');barra('cargaBarra',carga);let medidor=$('medidorBateria');medidor.style.setProperty('--nivel-carga',(percentual(carga)*3.6)+'deg');medidor.style.setProperty('--cor-carga',carga<20?'var(--vermelho)':carga<45?'var(--amarelo)':'var(--verde)');texto('bateriaDetalhe',(d.tensao_pacote_mv/1000).toFixed(2)+' V disponíveis no pacote');"
    "texto('potencia',d.potencia_w+' W');texto('tensao',(d.tensao_pacote_mv/1000).toFixed(2));texto('corrente',(d.corrente_centesimos_a/100).toFixed(2));texto('autonomia',(d.autonomia_decimos_km/10).toFixed(1)+' km');texto('percurso',(d.percurso_metros/1000).toFixed(3)+' km');"
    "texto('acelerador',acelerador.toFixed(1)+'%');texto('freio',freio.toFixed(1)+'%');barra('aceleradorBarra',acelerador);barra('freioBarra',freio);texto('frente',d.distancias_cm[0]+' cm');texto('traseira',d.distancias_cm[4]+' cm');mostrarCelulas(d.tensoes_celulas_mv);"
    "let mpuAtivo=(d.estado_mpu&1)!==0,mpuCalibrado=(d.estado_mpu&2)!==0,eixoOk=(d.estado_mpu&4)!==0,sentido=(d.estado_mpu>>3)&3,eixo=['X','Y','Z'][(d.estado_mpu>>5)&3]||'?';texto('mpuLongitudinal',mpuAtivo?(d.aceleracao_longitudinal_milesimos_g/1000).toFixed(3)+' g · '+(d.aceleracao_milesimos_ms2/1000).toFixed(3)+' m/s²':'Indisponível');texto('mpuSentido',mpuCalibrado?(sentido===1?'Para frente':sentido===2?'Para trás':'Neutro'):'Aguardando');texto('mpuAceleracao',mpuAtivo?[d.aceleracao_x_milesimos_g,d.aceleracao_y_milesimos_g,d.aceleracao_z_milesimos_g].map(x=>(x/1000).toFixed(3)).join(' / ')+' g':'—');texto('mpuGiro',mpuAtivo?[d.giroscopio_x_centesimos_graus_s,d.giroscopio_y_centesimos_graus_s,d.giroscopio_z_centesimos_graus_s].map(x=>(x/100).toFixed(2)).join(' / ')+' °/s':'—');texto('mpuTemperatura',mpuAtivo?(d.temperatura_mpu_centesimos_c/100).toFixed(1)+' °C':'—');texto('mpuEstado',!mpuAtivo?'Sem leitura':!eixoOk?'Verificar eixo '+eixo:mpuCalibrado?'Calibrado · eixo '+eixo:'Calibrando · eixo '+eixo);"
    "texto('latitude',(d.latitude_micrograus/1e6).toFixed(6)+'°');texto('longitude',(d.longitude_micrograus/1e6).toFixed(6)+'°');texto('rumo',(d.rumo_decimos_grau/10).toFixed(1)+'°');texto('odometro',(d.odometro_metros/1000).toFixed(2)+' km');texto('sequencia','#'+d.sequencia);texto('validos',d.pacotes_validos);texto('invalidos',d.pacotes_invalidos);texto('perdidos',d.pacotes_perdidos);texto('duplicados',d.pacotes_duplicados);texto('crcs',d.falhas_crc);texto('cabecalhos',d.falhas_cabecalho);texto('conteudos',d.falhas_conteudo);texto('reinicios',d.reinicios_mestre);"
    "texto('taxa',(d.taxa_pacotes_centesimos_hz/100).toFixed(2)+' pkt/s');texto('jitter',d.intervalo_medio_ms+' / '+d.jitter_medio_ms+' ms');texto('latencia',d.latencia_relativa_ms+' ms');texto('perda',(d.taxa_perda_centesimos_percentual/100).toFixed(2)+'%');texto('idade',d.idade_ms+' ms');classeNumero('invalidos',d.pacotes_invalidos,0);classeNumero('perdidos',d.pacotes_perdidos,0);classeNumero('duplicados',d.pacotes_duplicados,0);classeNumero('crcs',d.falhas_crc,0);classeNumero('cabecalhos',d.falhas_cabecalho,0);classeNumero('conteudos',d.falhas_conteudo,0);"
    "let recente=d.valido&&d.idade_ms<2000,estado=$('estado');estado.className='estado '+(recente?(d.simulado?'simulado':'online'):'offline');texto('estadoTexto',recente?(d.simulado?'Telemetria simulada ativa':'Telemetria real recebida'):'Sem telemetria recente');texto('fonte',d.simulado?'Dados gerados pelo simulador':'Sensores reais do veículo');texto('ultimaAtualizacao',recente?'Amostra com '+d.idade_ms+' ms · atualizada às '+new Date().toLocaleTimeString():'Última amostra com '+d.idade_ms+' ms');document.body.classList.toggle('offline',!recente)}"
    "async function atualizar(){if(consultando)return;consultando=true;try{let resposta=await fetch('/api/telemetria',{cache:'no-store'});if(!resposta.ok)throw new Error('HTTP '+resposta.status);aplicar(await resposta.json())}catch(erro){let estado=$('estado');estado.className='estado offline';texto('estadoTexto','Painel sem comunicação');texto('ultimaAtualizacao','Não foi possível consultar a API local');document.body.classList.add('offline')}finally{consultando=false}}"
    "$('atualizarAgora').onclick=atualizar;atualizar();setInterval(atualizar,250);</script></body></html>";

static const char pagina_diagnostico_html[] =
    "<!doctype html><html lang='pt-BR'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta name='theme-color' content='#050b12'><title>Diagnóstico | UFSM Carro</title><style>"
    ":root{color-scheme:dark;--fundo:#050b12;--painel:#102130;--borda:#20394d;--texto:#f7fbff;--fraco:#94aabd;--amarelo:#ffca46;--verde:#46dfa0;--vermelho:#ff687c;--azul:#4aaef1;--ciano:#36d2e2}*{box-sizing:border-box}html{background:var(--fundo)}body{margin:0;min-height:100vh;background:radial-gradient(circle at 88% -8%,#123d59 0,transparent 34rem),var(--fundo);color:var(--texto);font:15px system-ui,-apple-system,Segoe UI,sans-serif;font-feature-settings:'tnum' 1}body:before{content:'';position:fixed;inset:0;pointer-events:none;opacity:.11;background-image:linear-gradient(#fff1 1px,transparent 1px),linear-gradient(90deg,#fff1 1px,transparent 1px);background-size:48px 48px;mask-image:linear-gradient(to bottom,#000,transparent 70%)}"
    "main{position:relative;max-width:1260px;margin:auto;padding:25px 28px 38px}.topo{display:flex;justify-content:space-between;align-items:center;gap:18px;margin-bottom:24px}.marca{display:flex;align-items:center;gap:13px}.simbolo{display:grid;place-items:center;width:52px;height:52px;border-radius:17px;background:linear-gradient(145deg,#ffe080,#e5a329);color:#171109;font-size:.78rem;font-weight:900;letter-spacing:.04em;box-shadow:0 12px 34px #f5b94232}.sobretitulo,.rotulo{color:var(--fraco);font-size:.69rem;font-weight:750;letter-spacing:.1em;text-transform:uppercase}h1{font-size:1.48rem;line-height:1.1;margin:4px 0 0;letter-spacing:-.025em}nav{display:flex;gap:6px;padding:4px;background:#0b151fbb;border:1px solid var(--borda);border-radius:99px;backdrop-filter:blur(10px)}nav a{color:var(--fraco);text-decoration:none;font-weight:650;padding:9px 15px;border-radius:99px}nav a:hover{color:var(--texto);background:#ffffff0b}nav a.ativo{color:#171109;background:var(--amarelo)}"
    ".abertura{display:flex;align-items:center;justify-content:space-between;gap:20px;margin-bottom:17px;padding:20px 21px;border:1px solid var(--borda);border-radius:19px;background:linear-gradient(135deg,#0f2230,#091620);box-shadow:0 16px 40px #0003}.abertura h2{font-size:1.75rem;letter-spacing:-.035em;margin:5px 0}.abertura p{color:var(--fraco);line-height:1.5;margin:0;max-width:690px}.selo{display:flex;align-items:center;gap:9px;white-space:nowrap;padding:10px 13px;border:1px solid var(--borda);background:#081720;border-radius:99px;font-weight:700}.selo:before{content:'';width:8px;height:8px;border-radius:50%;background:var(--fraco)}.selo.ok{color:var(--verde);border-color:#2c6446}.selo.ok:before{background:var(--verde);box-shadow:0 0 12px var(--verde)}.selo.erro{color:var(--vermelho);border-color:#663740}.selo.erro:before{background:var(--vermelho)}"
    ".secao{margin-top:25px}.titulo-secao{display:flex;justify-content:space-between;align-items:end;gap:12px;margin-bottom:11px}.titulo-secao h2{font-size:1.05rem;margin:2px 0 0}.titulo-secao p{color:var(--fraco);font-size:.8rem;margin:0}.grade{display:grid;grid-template-columns:repeat(12,1fr);gap:12px}.cartao{position:relative;overflow:hidden;grid-column:span 3;min-height:125px;background:linear-gradient(145deg,#112432,#091721);border:1px solid var(--borda);border-radius:17px;padding:17px;box-shadow:0 14px 35px #0002}.cartao:before{content:'';position:absolute;left:16px;right:16px;top:0;height:2px;background:linear-gradient(90deg,var(--acento,var(--azul)),transparent)}.cartao[data-tom='ok']{--acento:var(--verde)}.cartao[data-tom='erro']{--acento:var(--vermelho)}.cartao[data-tom='alerta']{--acento:var(--amarelo)}.cartao.largo{grid-column:span 6}.valor{font-size:1.3rem;font-weight:760;line-height:1.25;letter-spacing:-.02em;margin-top:10px;overflow-wrap:anywhere}.detalhe{color:var(--fraco);font-size:.78rem;line-height:1.42;margin-top:7px}.memoria,.progresso{height:7px;margin-top:13px;background:#050e15;border-radius:99px;overflow:hidden}.memoria span,.progresso span{display:block;height:100%;width:0;background:linear-gradient(90deg,var(--ciano),var(--verde));border-radius:inherit;transition:width .3s}.legenda{display:flex;gap:15px;flex-wrap:wrap;color:var(--fraco);font-size:.75rem;margin-top:8px}.legenda b{color:var(--texto)}footer{display:flex;justify-content:space-between;gap:12px;color:var(--fraco);font-size:.78rem;margin-top:28px;padding-top:16px;border-top:1px solid #173044}"
    "@media(max-width:900px){.cartao{grid-column:span 6}.cartao.largo{grid-column:1/-1}}@media(max-width:620px){main{padding:17px 14px 29px}.topo,.abertura{align-items:flex-start;flex-direction:column}.topo{margin-bottom:21px}nav{width:100%;overflow:auto}nav a{flex:1;text-align:center;white-space:nowrap}.abertura{padding:17px}.cartao,.cartao.largo{grid-column:1/-1;min-height:112px}.titulo-secao{align-items:flex-start;flex-direction:column}.simbolo{width:44px;height:44px}footer{flex-direction:column}}@media(prefers-reduced-motion:reduce){*{transition:none!important}}"
    "</style></head><body><main><header class='topo'><div class='marca'><div class='simbolo'>CS</div><div><div class='sobretitulo'>UFSM-CS · ESP32-S3</div><h1>Painel da equipe</h1></div></div><nav aria-label='Navegação principal'><a href='http://painel.equipe/'>Equipe</a><a class='ativo' href='http://painel.diagnostico/'>Diagnóstico</a><a href='http://painel.mestre/'>Mestre</a></nav></header>"
    "<section class='abertura'><div><div class='sobretitulo'>Satélite da equipe</div><h2>Diagnóstico</h2><p>Estado local do ESP32, integridade da UART e serviços de comunicação. Os contadores são acumulados desde a inicialização.</p></div><div id='estado' class='selo' role='status' aria-live='polite'>Consultando</div></section>"
    "<section class='secao'><div class='titulo-secao'><div><div class='sobretitulo'>Sistema</div><h2>Execução e rede</h2></div><p>Identificação, memória e ponto de acesso</p></div><div class='grade'>"
    "<article id='cartaoVersao' class='cartao'><div class='rotulo'>Firmware</div><div id='versao' class='valor'>—</div><div id='compilacao' class='detalhe'>—</div></article><article class='cartao'><div class='rotulo'>Tempo ativo</div><div id='uptime' class='valor'>—</div><div class='detalhe'>Desde a última inicialização</div></article><article id='cartaoReinicio' class='cartao'><div class='rotulo'>Última inicialização</div><div id='reinicio' class='valor'>—</div><div class='detalhe'>Motivo informado pelo ESP-IDF</div></article><article class='cartao'><div class='rotulo'>Clientes Wi-Fi</div><div id='clientes' class='valor'>—</div><div class='detalhe'>Dispositivos conectados ao AP</div></article>"
    "<article id='cartaoMemoria' class='cartao largo'><div class='rotulo'>Memória interna</div><div id='heap' class='valor'>—</div><div class='memoria'><span id='heapBarra'></span></div><div class='legenda'><span>Livre <b id='heapLivre'>—</b></span><span>Mínimo <b id='heapMinimo'>—</b></span><span>Maior bloco <b id='heapMaior'>—</b></span></div></article><article class='cartao largo'><div class='rotulo'>Acesso local</div><div id='rede' class='valor'>—</div><div id='radio' class='detalhe'>—</div></article></div></section>"
    "<section class='secao'><div class='titulo-secao'><div><div class='sobretitulo'>Telemetria</div><h2>Qualidade dos dados</h2></div><p>Recepção e continuidade dos pacotes</p></div><div class='grade'>"
    "<article id='cartaoTelemetria' class='cartao'><div class='rotulo'>Amostra atual</div><div id='telemetriaEstado' class='valor'>—</div><div id='telemetriaDetalhe' class='detalhe'>—</div></article><article class='cartao'><div class='rotulo'>Taxa e período</div><div id='taxa' class='valor'>—</div><div class='detalhe'>Pacotes por segundo e intervalo médio</div></article><article id='cartaoPerda' class='cartao'><div class='rotulo'>Perda estimada</div><div id='perda' class='valor'>—</div><div class='detalhe'>Calculada pela sequência recebida</div></article><article class='cartao'><div class='rotulo'>Jitter e atraso</div><div id='atraso' class='valor'>—</div><div class='detalhe'>Variação / atraso relativo</div></article>"
    "<article id='cartaoPacotes' class='cartao largo'><div class='rotulo'>Integridade de protocolo</div><div id='pacotes' class='valor'>—</div><div id='pacotesDetalhe' class='detalhe'>—</div></article><article id='cartaoSequencia' class='cartao largo'><div class='rotulo'>Continuidade</div><div id='sequencia' class='valor'>—</div><div id='sequenciaDetalhe' class='detalhe'>—</div></article></div></section>"
    "<section class='secao'><div class='titulo-secao'><div><div class='sobretitulo'>UART</div><h2>Camada física</h2></div><p>Tráfego e erros do driver serial</p></div><div class='grade'>"
    "<article id='cartaoUart' class='cartao'><div class='rotulo'>Driver</div><div id='uartEstado' class='valor'>—</div><div class='detalhe'>Estado do gerenciador UART</div></article><article id='cartaoQuadros' class='cartao'><div class='rotulo'>Quadros TX / RX</div><div id='quadrosUart' class='valor'>—</div><div id='quadrosDetalhe' class='detalhe'>—</div></article><article class='cartao'><div class='rotulo'>Volume RX / TX</div><div id='bytes' class='valor'>—</div><div class='detalhe'>Bytes acumulados desde o boot</div></article><article id='cartaoFisicos' class='cartao'><div class='rotulo'>Erros físicos</div><div id='fisicos' class='valor'>—</div><div id='fisicosDetalhe' class='detalhe'>—</div></article><article id='cartaoErrosUart' class='cartao largo'><div class='rotulo'>Últimos resultados do driver</div><div id='errosUart' class='valor'>—</div><div class='detalhe'>Códigos ESP de transmissão / recepção</div></article><article id='cartaoValidacao' class='cartao largo'><div class='rotulo'>Falhas de validação</div><div id='validacao' class='valor'>—</div><div class='detalhe'>CRC / cabeçalho / conteúdo</div></article></div></section>"
    "<section class='secao'><div class='titulo-secao'><div><div class='sobretitulo'>Serviços</div><h2>Comandos e OTA local</h2></div><p>Operações entre o satélite e o mestre</p></div><div class='grade'>"
    "<article class='cartao largo'><div class='rotulo'>Comandos enviados</div><div id='comandosFluxo' class='valor'>—</div><div id='comandosDetalhe' class='detalhe'>—</div></article><article id='cartaoComandosErro' class='cartao largo'><div class='rotulo'>Confiabilidade dos comandos</div><div id='comandosErros' class='valor'>—</div><div id='comandosUltimo' class='detalhe'>—</div></article><article id='cartaoOtaLocal' class='cartao largo'><div class='rotulo'>Recepção OTA do satélite</div><div id='otaEstado' class='valor'>—</div><div class='progresso'><span id='otaBarra'></span></div><div id='otaDetalhe' class='detalhe'>—</div></article><article class='cartao largo'><div class='rotulo'>Firmware de destino</div><div id='otaVersao' class='valor'>—</div><div class='detalhe'>Versão anunciada na sessão OTA atual</div></article></div></section>"
    "<footer><span>Atualização automática a cada segundo</span><span>Acesso local · painel.equipe</span></footer></main><script>"
    "const $=id=>document.getElementById(id);let consultando=false;const motivos={0:'Desconhecido',1:'Energia ligada',2:'Reset externo',3:'Reinício por software',4:'Exceção ou pânico',5:'Watchdog interno',6:'Watchdog de tarefa',7:'Watchdog',8:'Saída de sono profundo',9:'Queda de tensão',10:'Reset SDIO'};function texto(id,valor){$(id).textContent=valor}function tempo(ms){let s=Math.floor((Number(ms)||0)/1000),d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return d?d+' d '+h+' h':h?h+' h '+m+' min':m?m+' min '+s%60+' s':s+' s'}function bytes(v){v=Number(v)||0;return v>=1048576?(v/1048576).toFixed(2)+' MiB':v>=1024?(v/1024).toFixed(1)+' KiB':v+' B'}function erro(v){return Number(v)===0?'ESP_OK':String(v)}function tom(id,valor){$(id).dataset.tom=valor||''}"
    "function aplicar(d){let recente=d.valido&&d.idade_ms<2000,estado=$('estado');estado.textContent=recente?'Enlace ativo · '+d.idade_ms+' ms':'Sem telemetria recente';estado.className='selo '+(recente?'ok':'erro');texto('versao',d.versao_firmware+' · protocolo v'+d.versao_protocolo);texto('compilacao','Compilado em '+d.compilacao_data+' às '+d.compilacao_hora);texto('uptime',tempo(d.tempo_ativo_ms));texto('reinicio',motivos[d.motivo_reinicio]||('Código '+d.motivo_reinicio));tom('cartaoReinicio',[4,5,6,7,9].includes(d.motivo_reinicio)?'alerta':'ok');texto('clientes',d.clientes_wifi);texto('rede','http://'+d.nome_local);texto('radio','Canal '+d.wifi_canal+' · potência máxima '+(d.wifi_potencia_quartos_dbm/4).toFixed(2)+' dBm');"
    "let livre=d.heap_livre/1024,minimo=d.heap_minimo/1024,maior=d.heap_maior_bloco/1024,razao=d.heap_livre?d.heap_maior_bloco/d.heap_livre:0;texto('heap',livre.toFixed(1)+' KiB livres');texto('heapLivre',livre.toFixed(1)+' KiB');texto('heapMinimo',minimo.toFixed(1)+' KiB');texto('heapMaior',maior.toFixed(1)+' KiB');$('heapBarra').style.width=Math.max(4,Math.min(100,livre/320*100))+'%';tom('cartaoMemoria',livre<32||razao<.25?'erro':livre<64||razao<.45?'alerta':'ok');"
    "texto('telemetriaEstado',recente?'Atual':'Atrasada');texto('telemetriaDetalhe','Sequência #'+d.sequencia+' · idade '+d.idade_ms+' ms');tom('cartaoTelemetria',recente?'ok':'erro');texto('taxa',(d.taxa_pacotes_centesimos_hz/100).toFixed(2)+' pkt/s · '+d.intervalo_medio_ms+' ms');let perda=d.taxa_perda_centesimos_percentual/100;texto('perda',perda.toFixed(2)+'%');tom('cartaoPerda',perda>2?'erro':perda>0?'alerta':'ok');texto('atraso',d.jitter_medio_ms+' / '+d.latencia_relativa_ms+' ms');texto('pacotes',d.pacotes_validos+' válidos · '+d.pacotes_invalidos+' inválidos');texto('pacotesDetalhe','CRC '+d.falhas_crc+' · cabeçalho '+d.falhas_cabecalho+' · conteúdo '+d.falhas_conteudo);tom('cartaoPacotes',d.pacotes_invalidos?'erro':'ok');texto('sequencia',d.pacotes_perdidos+' perdidos · '+d.pacotes_duplicados+' duplicados');texto('sequenciaDetalhe',d.reinicios_mestre+' reinício(s) do mestre detectado(s)');tom('cartaoSequencia',d.pacotes_perdidos||d.pacotes_duplicados?'alerta':'ok');"
    "texto('uartEstado',d.uart_inicializada?'Inicializada':'Indisponível');tom('cartaoUart',d.uart_inicializada?'ok':'erro');texto('quadrosUart',d.uart_envios_ok+' / '+d.uart_recepcoes_ok);texto('quadrosDetalhe',d.uart_erros_envio+' erro(s) TX · '+d.uart_erros_recepcao+' erro(s) RX');tom('cartaoQuadros',d.uart_erros_envio||d.uart_erros_recepcao?'erro':'ok');texto('bytes',bytes(d.bytes_recebidos)+' / '+bytes(d.bytes_enviados));let fisicos=d.uart_estouros_fifo+d.uart_buffers_cheios+d.uart_erros_quadro+d.uart_erros_paridade+d.uart_sinais_break;texto('fisicos',fisicos);texto('fisicosDetalhe','FIFO '+d.uart_estouros_fifo+' · buffer '+d.uart_buffers_cheios+' · quadro '+d.uart_erros_quadro+' · paridade '+d.uart_erros_paridade+' · break '+d.uart_sinais_break);tom('cartaoFisicos',fisicos?'erro':'ok');texto('errosUart',erro(d.uart_ultimo_erro_envio)+' / '+erro(d.uart_ultimo_erro_recepcao));tom('cartaoErrosUart',d.uart_ultimo_erro_envio||d.uart_ultimo_erro_recepcao?'alerta':'ok');texto('validacao',d.falhas_crc+' / '+d.falhas_cabecalho+' / '+d.falhas_conteudo);tom('cartaoValidacao',d.falhas_crc+d.falhas_cabecalho+d.falhas_conteudo?'erro':'ok');"
    "texto('comandosFluxo',d.comandos_solicitacoes_enviadas+' solicitações · '+d.comandos_ok+' respostas');texto('comandosDetalhe',d.comandos_solicitacoes_recebidas+' recebidas do mestre · '+d.comandos_respostas_enviadas+' respondidas');texto('comandosErros',d.comandos_timeout+' timeout(s) · '+d.comandos_invalidos+' inválido(s) · '+d.comandos_repeticoes+' repetição(ões)');texto('comandosUltimo','Último resultado: '+erro(d.comandos_ultimo_erro));tom('cartaoComandosErro',d.comandos_timeout||d.comandos_invalidos||d.comandos_ultimo_erro?'erro':'ok');let progresso=d.ota_tamanho_total?d.ota_bytes_recebidos/d.ota_tamanho_total*100:0;texto('otaEstado',d.ota_recebendo?'Recebendo firmware':d.ota_iniciado?'Pronta':'Não inicializada');$('otaBarra').style.width=Math.max(0,Math.min(100,progresso))+'%';texto('otaDetalhe',progresso.toFixed(1)+'% · sessão '+d.ota_sessao+' · '+d.ota_quadros_recebidos+' quadro(s) · '+d.ota_falhas+' falha(s) · '+bytes(d.ota_bytes_recebidos));texto('otaVersao',d.ota_versao_destino||'Nenhuma sessão ativa');tom('cartaoOtaLocal',d.ota_falhas||d.ota_ultimo_erro?'erro':d.ota_recebendo?'alerta':'ok')}"
    "async function atualizar(){if(consultando)return;consultando=true;try{let resposta=await fetch('/api/diagnostico',{cache:'no-store'});if(!resposta.ok)throw new Error('HTTP '+resposta.status);aplicar(await resposta.json())}catch(erro){let estado=$('estado');estado.textContent='Diagnóstico indisponível';estado.className='selo erro'}finally{consultando=false}}atualizar();setInterval(atualizar,1000);</script></body></html>";

static bool requisicao_usa_host(httpd_req_t *requisicao, const char *esperado)
{
    char host[64] = {0};
    if (httpd_req_get_hdr_value_str(requisicao, "Host", host,
                                    sizeof(host)) != ESP_OK) {
        return false;
    }
    char *porta = strchr(host, ':');
    if (porta != NULL) *porta = '\0';
    return strcasecmp(host, esperado) == 0;
}

static const char *pagina_para_host(httpd_req_t *requisicao)
{
    if (requisicao_usa_host(requisicao, NOME_LOCAL_MESTRE)) {
        return PAGINA_MESTRE_WEB;
    }
    if (requisicao_usa_host(requisicao, NOME_LOCAL_DIAGNOSTICO)) {
        return pagina_diagnostico_html;
    }
    return pagina_html;
}

static const char *url_para_host(httpd_req_t *requisicao)
{
    if (requisicao_usa_host(requisicao, NOME_LOCAL_MESTRE)) {
        return URL_LOCAL_MESTRE;
    }
    if (requisicao_usa_host(requisicao, NOME_LOCAL_DIAGNOSTICO)) {
        return URL_LOCAL_DIAGNOSTICO;
    }
    return URL_LOCAL_EQUIPE;
}

static esp_err_t responder_painel(httpd_req_t *requisicao)
{
    httpd_resp_set_hdr(requisicao, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_type(requisicao, "text/html; charset=utf-8");
    return httpd_resp_send(requisicao, pagina_para_host(requisicao),
                           HTTPD_RESP_USE_STRLEN);
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
    if (strcmp(consulta, "configuracao") == 0)
        return COMANDO_OBTER_CONFIGURACAO_MESTRE;
    if (strcmp(consulta, "rede_wifi_salva") == 0)
        return COMANDO_OBTER_REDE_WIFI_SALVA;
    if (strcmp(consulta, "satelite_equipe_ping") == 0 ||
        strcmp(consulta, "satelite_equipe_versao") == 0 ||
        strcmp(consulta, "satelite_visitantes_ping") == 0 ||
        strcmp(consulta, "satelite_visitantes_versao") == 0)
        return COMANDO_CONSULTAR_SATELITE;
    if (strcmp(consulta, "verificar_ota") == 0)
        return COMANDO_SOLICITAR_VERIFICACAO_OTA;
    if (strcmp(consulta, "autorizar_mestre") == 0)
        return COMANDO_AUTORIZAR_OTA_MESTRE;
    if (strcmp(consulta, "autorizar_equipe") == 0)
        return COMANDO_AUTORIZAR_OTA_EQUIPE;
    if (strcmp(consulta, "autorizar_visitantes") == 0)
        return COMANDO_AUTORIZAR_OTA_VISITANTES;
    if (strcmp(consulta, "autorizar_todos") == 0)
        return COMANDO_AUTORIZAR_OTA_TODOS;
    if (strcmp(consulta, "cancelar_ota") == 0) return COMANDO_CANCELAR_OTA;
    if (strcmp(consulta, "resumo_terminal") == 0)
        return COMANDO_SOLICITAR_RESUMO_TERMINAL;
    if (strcmp(consulta, "alternar_relatorios") == 0 ||
        strcmp(consulta, "zerar_percurso") == 0 ||
        strcmp(consulta, "sincronizar_google") == 0 ||
        strcmp(consulta, "intervalo_relatorio") == 0 ||
        strcmp(consulta, "intervalo_ota") == 0 ||
        strcmp(consulta, "intervalo_telemetria_ota") == 0 ||
        strcmp(consulta, "wifi_preparar") == 0 ||
        strcmp(consulta, "wifi_conectar_preparada") == 0 ||
        strcmp(consulta, "wifi_salvar_preparada") == 0 ||
        strcmp(consulta, "wifi_conectar_salva") == 0 ||
        strcmp(consulta, "wifi_excluir_salva") == 0 ||
        strcmp(consulta, "wifi_automatico") == 0 ||
        strcmp(consulta, "wifi_alterar_principal") == 0 ||
        strcmp(consulta, "wifi_restaurar_principal") == 0 ||
        strcmp(consulta, "wifi_politica") == 0) {
        return COMANDO_EXECUTAR_ACAO_MESTRE;
    }
    return 0;
}

static bool comando_exige_confirmacao(codigo_comando_t comando)
{
    switch (comando) {
        case COMANDO_SOLICITAR_VERIFICACAO_OTA:
        case COMANDO_AUTORIZAR_OTA_MESTRE:
        case COMANDO_AUTORIZAR_OTA_EQUIPE:
        case COMANDO_AUTORIZAR_OTA_VISITANTES:
        case COMANDO_AUTORIZAR_OTA_TODOS:
        case COMANDO_CANCELAR_OTA:
        case COMANDO_SOLICITAR_RESUMO_TERMINAL:
        case COMANDO_EXECUTAR_ACAO_MESTRE:
            return true;
        default:
            return false;
    }
}

static acao_mestre_t localizar_acao_mestre(const char *consulta)
{
    if (strcmp(consulta, "alternar_relatorios") == 0)
        return ACAO_MESTRE_ALTERNAR_RELATORIOS;
    if (strcmp(consulta, "zerar_percurso") == 0)
        return ACAO_MESTRE_ZERAR_PERCURSO;
    if (strcmp(consulta, "sincronizar_google") == 0)
        return ACAO_MESTRE_SINCRONIZAR_GOOGLE;
    if (strcmp(consulta, "intervalo_relatorio") == 0)
        return ACAO_MESTRE_DEFINIR_INTERVALO_RELATORIO;
    if (strcmp(consulta, "intervalo_ota") == 0)
        return ACAO_MESTRE_DEFINIR_INTERVALO_OTA;
    if (strcmp(consulta, "intervalo_telemetria_ota") == 0)
        return ACAO_MESTRE_DEFINIR_INTERVALO_TELEMETRIA_OTA;
    if (strcmp(consulta, "wifi_preparar") == 0)
        return ACAO_MESTRE_PREPARAR_REDE_WIFI;
    if (strcmp(consulta, "wifi_conectar_preparada") == 0)
        return ACAO_MESTRE_CONECTAR_REDE_WIFI_PREPARADA;
    if (strcmp(consulta, "wifi_salvar_preparada") == 0)
        return ACAO_MESTRE_SALVAR_REDE_WIFI_PREPARADA;
    if (strcmp(consulta, "wifi_conectar_salva") == 0)
        return ACAO_MESTRE_CONECTAR_REDE_WIFI_SALVA;
    if (strcmp(consulta, "wifi_excluir_salva") == 0)
        return ACAO_MESTRE_EXCLUIR_REDE_WIFI_SALVA;
    if (strcmp(consulta, "wifi_automatico") == 0)
        return ACAO_MESTRE_DEFINIR_WIFI_AUTOMATICO;
    if (strcmp(consulta, "wifi_alterar_principal") == 0)
        return ACAO_MESTRE_ALTERAR_REDE_WIFI_PRINCIPAL;
    if (strcmp(consulta, "wifi_restaurar_principal") == 0)
        return ACAO_MESTRE_RESTAURAR_REDE_WIFI_PRINCIPAL;
    if (strcmp(consulta, "wifi_politica") == 0)
        return ACAO_MESTRE_DEFINIR_POLITICA_WIFI;
    return 0;
}

static bool converter_uint32(const char *texto, uint32_t *valor)
{
    if (texto == NULL || valor == NULL || texto[0] == '\0') return false;
    char *fim = NULL;
    const unsigned long numero = strtoul(texto, &fim, 10);
    if (fim == texto || *fim != '\0' || numero > UINT32_MAX) return false;
    *valor = (uint32_t)numero;
    return true;
}

static char *proximo_campo(char **cursor)
{
    if (cursor == NULL || *cursor == NULL) return NULL;
    char *campo = *cursor;
    char *fim = strchr(campo, '\n');
    if (fim == NULL) {
        *cursor = NULL;
    } else {
        *fim = '\0';
        *cursor = fim + 1;
    }
    return campo;
}

static esp_err_t receber_corpo_texto(httpd_req_t *requisicao, char *corpo,
                                     size_t capacidade)
{
    if (capacidade == 0 ||
        (size_t)requisicao->content_len >= capacidade) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t recebido = 0;
    while (recebido < (size_t)requisicao->content_len) {
        const int trecho = httpd_req_recv(
            requisicao, corpo + recebido,
            (size_t)requisicao->content_len - recebido);
        if (trecho <= 0) return ESP_FAIL;
        recebido += (size_t)trecho;
    }
    corpo[recebido] = '\0';
    return ESP_OK;
}

static bool preparar_acao_mestre(const char *consulta, char *corpo,
                                 solicitacao_acao_mestre_t *solicitacao)
{
    *solicitacao = (solicitacao_acao_mestre_t){0};
    solicitacao->acao = (uint8_t)localizar_acao_mestre(consulta);
    if (solicitacao->acao == 0) return false;
    char *cursor = corpo;
    char *campo = NULL;
    uint32_t valor = 0;
    switch ((acao_mestre_t)solicitacao->acao) {
        case ACAO_MESTRE_DEFINIR_INTERVALO_RELATORIO:
            campo = proximo_campo(&cursor);
            if (!converter_uint32(campo, &valor) || valor > 3) return false;
            solicitacao->alvo = (uint8_t)valor;
            campo = proximo_campo(&cursor);
            if (!converter_uint32(campo, &valor)) return false;
            solicitacao->valor = valor;
            return true;
        case ACAO_MESTRE_DEFINIR_INTERVALO_OTA:
        case ACAO_MESTRE_DEFINIR_INTERVALO_TELEMETRIA_OTA:
            if (!converter_uint32(proximo_campo(&cursor), &valor))
                return false;
            solicitacao->valor = valor;
            return true;
        case ACAO_MESTRE_PREPARAR_REDE_WIFI: {
            const char *ssid = proximo_campo(&cursor);
            const char *senha = proximo_campo(&cursor);
            if (ssid == NULL || senha == NULL || strlen(ssid) > 32 ||
                strlen(senha) > 63 ||
                strlen(ssid) + strlen(senha) + 2 >
                    sizeof(solicitacao->texto)) return false;
            strcpy(solicitacao->texto, ssid);
            strcpy(solicitacao->texto + strlen(ssid) + 1, senha);
            return true;
        }
        case ACAO_MESTRE_CONECTAR_REDE_WIFI_SALVA:
        case ACAO_MESTRE_EXCLUIR_REDE_WIFI_SALVA:
            if (!converter_uint32(proximo_campo(&cursor), &valor) ||
                valor < 1 || valor > 5) return false;
            solicitacao->alvo = (uint8_t)(valor - 1);
            return true;
        case ACAO_MESTRE_DEFINIR_WIFI_AUTOMATICO:
            if (!converter_uint32(proximo_campo(&cursor), &valor) ||
                valor > 1) return false;
            solicitacao->valor = valor;
            campo = proximo_campo(&cursor);
            if (campo == NULL || strlen(campo) >= sizeof(solicitacao->texto))
                return false;
            strcpy(solicitacao->texto, campo);
            return true;
        case ACAO_MESTRE_ALTERAR_REDE_WIFI_PRINCIPAL:
        case ACAO_MESTRE_RESTAURAR_REDE_WIFI_PRINCIPAL:
            campo = proximo_campo(&cursor);
            if (campo == NULL || strlen(campo) >= sizeof(solicitacao->texto))
                return false;
            strcpy(solicitacao->texto, campo);
            return true;
        case ACAO_MESTRE_DEFINIR_POLITICA_WIFI: {
            uint32_t valores[4] = {0};
            for (size_t indice = 0; indice < 4; ++indice) {
                if (!converter_uint32(proximo_campo(&cursor),
                                      &valores[indice])) return false;
            }
            solicitacao->valor = valores[0];
            solicitacao->valor_2 = valores[1];
            solicitacao->valor_3 = valores[2];
            solicitacao->valor_4 = valores[3];
            campo = proximo_campo(&cursor);
            if (campo == NULL || strlen(campo) >= sizeof(solicitacao->texto))
                return false;
            strcpy(solicitacao->texto, campo);
            return true;
        }
        default:
            return corpo[0] == '\0';
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

    const void *carga_solicitacao = NULL;
    uint16_t tamanho_carga_solicitacao = 0;
    solicitacao_acao_mestre_t solicitacao_acao = {0};
    solicitacao_rede_wifi_salva_t solicitacao_rede = {0};
    solicitacao_consulta_satelite_t solicitacao_satelite = {0};
    char corpo[256] = {0};
    TickType_t tempo_limite = pdMS_TO_TICKS(1500);
    if (comando == COMANDO_EXECUTAR_ACAO_MESTRE) {
        if (receber_corpo_texto(requisicao, corpo, sizeof(corpo)) != ESP_OK ||
            !preparar_acao_mestre(consulta, corpo, &solicitacao_acao)) {
            return enviar_json_comando(
                requisicao, "400 Bad Request",
                "{\"ok\":false,\"erro\":\"dados da ação inválidos\"}");
        }
        carga_solicitacao = &solicitacao_acao;
        tamanho_carga_solicitacao = sizeof(solicitacao_acao);
        if (solicitacao_acao.acao ==
                ACAO_MESTRE_CONECTAR_REDE_WIFI_PREPARADA ||
            solicitacao_acao.acao == ACAO_MESTRE_CONECTAR_REDE_WIFI_SALVA ||
            solicitacao_acao.acao ==
                ACAO_MESTRE_ALTERAR_REDE_WIFI_PRINCIPAL) {
            // A política do mestre permite sessões Wi-Fi de até 180 s.
            tempo_limite = pdMS_TO_TICKS(185000);
        }
    } else if (comando == COMANDO_OBTER_REDE_WIFI_SALVA) {
        char indice_texto[8] = {0};
        uint32_t indice = 0;
        if (httpd_query_key_value(consulta_url, "indice", indice_texto,
                                  sizeof(indice_texto)) != ESP_OK ||
            !converter_uint32(indice_texto, &indice) || indice < 1 ||
            indice > 5) {
            return enviar_json_comando(
                requisicao, "400 Bad Request",
                "{\"ok\":false,\"erro\":\"índice de rede inválido\"}");
        }
        solicitacao_rede.indice = (uint8_t)(indice - 1);
        carga_solicitacao = &solicitacao_rede;
        tamanho_carga_solicitacao = sizeof(solicitacao_rede);
    } else if (comando == COMANDO_CONSULTAR_SATELITE) {
        solicitacao_satelite.alvo = strstr(consulta, "visitantes") != NULL
                                         ? NO_COMANDO_VISITANTES
                                         : NO_COMANDO_EQUIPE;
        solicitacao_satelite.tipo = strstr(consulta, "versao") != NULL
                                        ? CONSULTA_SATELITE_VERSAO
                                        : CONSULTA_SATELITE_PING;
        carga_solicitacao = &solicitacao_satelite;
        tamanho_carga_solicitacao = sizeof(solicitacao_satelite);
        tempo_limite = pdMS_TO_TICKS(3500);
    }

    uint8_t resposta[COMANDO_TAMANHO_MAXIMO_CARGA] = {0};
    uint16_t tamanho_resposta = 0;
    codigo_resposta_comando_t resultado = RESPOSTA_COMANDO_TIMEOUT;
    informacoes_solicitacao_comando_t informacoes = {0};
    const esp_err_t erro = servico_comandos_satelite_solicitar_detalhado(
        comando, carga_solicitacao, tamanho_carga_solicitacao, resposta,
        sizeof(resposta), &tamanho_resposta, &resultado, tempo_limite,
        &informacoes);
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
    } else if (comando == COMANDO_OBTER_CONFIGURACAO_MESTRE &&
               tamanho_resposta == sizeof(resposta_configuracao_mestre_t)) {
        resposta_configuracao_mestre_t dados;
        memcpy(&dados, resposta, sizeof(dados));
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"configuracao\",\"comando\":%u,"
            "\"solicitacao\":%lu,\"resultado\":0,\"tentativas\":%u,"
            "\"duracao_ms\":%lu,\"relatorios_pausados\":%s,"
            "\"intervalos_relatorios_ms\":[%lu,%lu,%lu,%lu],"
            "\"intervalo_ota_minutos\":%lu,\"intervalo_telemetria_ota_ms\":%lu,"
            "\"cartao_disponivel\":%s,\"amostras_sd\":%lu,"
            "\"arquivos_pendentes\":%lu,\"arquivos_enviados\":%lu,"
            "\"quantidade_redes_wifi\":%u,\"wifi_automatico\":%s,"
            "\"tentativas_wifi\":%u,\"tempo_tentativa_wifi_ms\":%lu,"
            "\"intervalo_tentativas_wifi_ms\":%lu,\"tempo_limite_wifi_ms\":%lu}",
            (unsigned)comando, (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms,
            dados.relatorios_pausados ? "true" : "false",
            (unsigned long)dados.intervalos_relatorios_ms[0],
            (unsigned long)dados.intervalos_relatorios_ms[1],
            (unsigned long)dados.intervalos_relatorios_ms[2],
            (unsigned long)dados.intervalos_relatorios_ms[3],
            (unsigned long)dados.intervalo_ota_minutos,
            (unsigned long)dados.intervalo_telemetria_ota_ms,
            dados.cartao_disponivel ? "true" : "false",
            (unsigned long)dados.amostras_sd,
            (unsigned long)dados.arquivos_pendentes,
            (unsigned long)dados.arquivos_enviados,
            (unsigned)dados.quantidade_redes_wifi,
            dados.wifi_automatico ? "true" : "false",
            (unsigned)dados.tentativas_wifi,
            (unsigned long)dados.tempo_tentativa_wifi_ms,
            (unsigned long)dados.intervalo_tentativas_wifi_ms,
            (unsigned long)dados.tempo_limite_wifi_ms);
    } else if (comando == COMANDO_OBTER_REDE_WIFI_SALVA &&
               tamanho_resposta == sizeof(resposta_rede_wifi_salva_t)) {
        resposta_rede_wifi_salva_t dados;
        char ssid[67];
        memcpy(&dados, resposta, sizeof(dados));
        dados.ssid[sizeof(dados.ssid) - 1] = '\0';
        escapar_texto_json(ssid, sizeof(ssid), dados.ssid);
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"rede_wifi_salva\",\"comando\":%u,"
            "\"solicitacao\":%lu,\"resultado\":0,\"tentativas\":%u,"
            "\"duracao_ms\":%lu,\"indice\":%u,\"ssid\":\"%s\"}",
            (unsigned)comando, (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms,
            (unsigned)dados.indice + 1, ssid);
    } else if (comando == COMANDO_CONSULTAR_SATELITE &&
               tamanho_resposta == sizeof(resposta_consulta_satelite_t)) {
        resposta_consulta_satelite_t dados;
        char versao[65];
        memcpy(&dados, resposta, sizeof(dados));
        dados.versao[sizeof(dados.versao) - 1] = '\0';
        escapar_texto_json(versao, sizeof(versao), dados.versao);
        tamanho = snprintf(json, sizeof(json),
            "{\"ok\":true,\"consulta\":\"%s\",\"comando\":%u,"
            "\"solicitacao\":%lu,\"resultado\":0,\"tentativas\":%u,"
            "\"duracao_ms\":%lu,\"alvo\":%u,\"tipo\":%u,"
            "\"rtt_ms\":%lu,\"tempo_ativo_ms\":%lu,"
            "\"capacidades\":%lu,\"firmware\":\"%s\"}",
            consulta, (unsigned)comando,
            (unsigned long)informacoes.identificador,
            (unsigned)informacoes.tentativas,
            (unsigned long)informacoes.duracao_ms, (unsigned)dados.alvo,
            (unsigned)dados.tipo, (unsigned long)dados.rtt_ms,
            (unsigned long)dados.tempo_ativo_ms,
            (unsigned long)dados.capacidades, versao);
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
    char json[2048];
    int tamanho = snprintf(json, sizeof(json),
        "{\"valido\":%s,\"idade_ms\":%lu,\"sequencia\":%u,\"tempo_mestre_ms\":%lu,\"tensao_adc_bruta\":%u,"
        "\"aceleracao_x\":%d,\"aceleracao_y\":%d,\"aceleracao_z\":%d,\"giroscopio_x\":%d,\"giroscopio_y\":%d,\"giroscopio_z\":%d,"
        "\"aceleracao_x_milesimos_g\":%d,\"aceleracao_y_milesimos_g\":%d,\"aceleracao_z_milesimos_g\":%d,"
        "\"giroscopio_x_centesimos_graus_s\":%d,\"giroscopio_y_centesimos_graus_s\":%d,\"giroscopio_z_centesimos_graus_s\":%d,"
        "\"aceleracao_longitudinal_milesimos_g\":%d,\"temperatura_mpu_centesimos_c\":%d,\"estado_mpu\":%u,\"pacotes_validos\":%lu,\"pacotes_invalidos\":%lu,"
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
        dados.giroscopio_z,
        dados.aceleracao_x_milesimos_g, dados.aceleracao_y_milesimos_g,
        dados.aceleracao_z_milesimos_g,
        dados.giroscopio_x_centesimos_graus_s,
        dados.giroscopio_y_centesimos_graus_s,
        dados.giroscopio_z_centesimos_graus_s,
        dados.aceleracao_longitudinal_milesimos_g,
        dados.temperatura_mpu_centesimos_c, (unsigned)dados.estado_mpu,
        (unsigned long)dados.pacotes_validos, (unsigned long)dados.pacotes_invalidos,
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
    httpd_resp_set_hdr(requisicao, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_type(requisicao, "text/html; charset=utf-8");
    return httpd_resp_send(requisicao, pagina_diagnostico_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t responder_diagnostico_api(httpd_req_t *requisicao)
{
    dados_telemetria_t dados = {0};
    estatisticas_gerenciador_uart_t uart = {0};
    const estatisticas_servico_comandos_satelite_t comandos =
        servico_comandos_satelite_obter_estatisticas();
    const situacao_servico_ota_satelite_t ota =
        servico_ota_satelite_obter_situacao();
    wifi_sta_list_t clientes = {0};
    uint8_t canal_wifi = 0;
    wifi_second_chan_t canal_secundario = WIFI_SECOND_CHAN_NONE;
    int8_t potencia_wifi = 0;
    receptor_uart_obter_dados(&dados);
    gerenciador_uart_obter_estatisticas(&uart);
    (void)esp_wifi_ap_get_sta_list(&clientes);
    (void)esp_wifi_get_channel(&canal_wifi, &canal_secundario);
    (void)esp_wifi_get_max_tx_power(&potencia_wifi);
    const uint32_t agora_ms = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t idade_ms = dados.ultimo_recebimento_ms == 0
                                  ? UINT32_MAX : agora_ms - dados.ultimo_recebimento_ms;
    char versao_ota[sizeof(ota.versao_destino) * 2 + 1];
    escapar_texto_json(versao_ota, sizeof(versao_ota), ota.versao_destino);
    char json[3072];
    const int tamanho = snprintf(json, sizeof(json),
        "{\"valido\":%s,\"idade_ms\":%lu,\"versao_firmware\":\"%s\",\"versao_protocolo\":%u,"
        "\"compilacao_data\":\"%s\",\"compilacao_hora\":\"%s\",\"tempo_ativo_ms\":%lu,\"motivo_reinicio\":%u,"
        "\"heap_livre\":%lu,\"heap_minimo\":%lu,\"heap_maior_bloco\":%lu,\"clientes_wifi\":%u,"
        "\"nome_local\":\"%s\",\"wifi_canal\":%u,\"wifi_potencia_quartos_dbm\":%d,"
        "\"sequencia\":%u,\"reinicios_mestre\":%lu,\"taxa_perda_centesimos_percentual\":%lu,"
        "\"taxa_pacotes_centesimos_hz\":%lu,\"intervalo_medio_ms\":%lu,\"jitter_medio_ms\":%lu,\"latencia_relativa_ms\":%lu,"
        "\"pacotes_validos\":%lu,\"pacotes_invalidos\":%lu,\"pacotes_perdidos\":%lu,\"pacotes_duplicados\":%lu,"
        "\"falhas_crc\":%lu,\"falhas_cabecalho\":%lu,\"falhas_conteudo\":%lu,"
        "\"uart_inicializada\":%s,\"uart_envios_ok\":%lu,\"uart_erros_envio\":%lu,\"uart_recepcoes_ok\":%lu,\"uart_erros_recepcao\":%lu,"
        "\"bytes_recebidos\":%llu,\"bytes_enviados\":%llu,\"uart_estouros_fifo\":%lu,\"uart_buffers_cheios\":%lu,"
        "\"uart_erros_quadro\":%lu,\"uart_erros_paridade\":%lu,\"uart_sinais_break\":%lu,\"uart_ultimo_erro_envio\":%ld,\"uart_ultimo_erro_recepcao\":%ld,"
        "\"comandos_solicitacoes_recebidas\":%lu,\"comandos_respostas_enviadas\":%lu,\"comandos_solicitacoes_enviadas\":%lu,"
        "\"comandos_ok\":%lu,\"comandos_repeticoes\":%lu,\"comandos_timeout\":%lu,\"comandos_invalidos\":%lu,\"comandos_ultimo_erro\":%ld,"
        "\"ota_iniciado\":%s,\"ota_recebendo\":%s,\"ota_sessao\":%lu,\"ota_tamanho_total\":%lu,\"ota_bytes_recebidos\":%lu,"
        "\"ota_quadros_recebidos\":%lu,\"ota_falhas\":%lu,\"ota_ultimo_erro\":%ld,\"ota_versao_destino\":\"%s\"}",
        dados.valido ? "true" : "false", (unsigned long)idade_ms,
        esp_app_get_description()->version,
        (unsigned)TELEMETRIA_VERSAO_PROTOCOLO,
        esp_app_get_description()->date, esp_app_get_description()->time,
        (unsigned long)agora_ms, (unsigned)esp_reset_reason(),
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)esp_get_minimum_free_heap_size(),
        (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        (unsigned)clientes.num, NOME_LOCAL_EQUIPE, (unsigned)canal_wifi,
        (int)potencia_wifi, (unsigned)dados.sequencia,
        (unsigned long)dados.reinicios_mestre,
        (unsigned long)dados.taxa_perda_centesimos_percentual,
        (unsigned long)dados.taxa_pacotes_centesimos_hz,
        (unsigned long)dados.intervalo_medio_ms, (unsigned long)dados.jitter_medio_ms,
        (unsigned long)dados.latencia_relativa_ms,
        (unsigned long)dados.pacotes_validos, (unsigned long)dados.pacotes_invalidos,
        (unsigned long)dados.pacotes_perdidos, (unsigned long)dados.pacotes_duplicados,
        (unsigned long)dados.falhas_crc, (unsigned long)dados.falhas_cabecalho,
        (unsigned long)dados.falhas_conteudo,
        uart.inicializado ? "true" : "false",
        (unsigned long)uart.envios_ok, (unsigned long)uart.erros_envio,
        (unsigned long)uart.recepcoes_ok, (unsigned long)uart.erros_recepcao,
        (unsigned long long)uart.bytes_recebidos, (unsigned long long)uart.bytes_enviados,
        (unsigned long)uart.estouros_fifo, (unsigned long)uart.buffers_cheios,
        (unsigned long)uart.erros_quadro, (unsigned long)uart.erros_paridade,
        (unsigned long)uart.sinais_break, (long)uart.ultimo_erro_envio,
        (long)uart.ultimo_erro_recepcao,
        (unsigned long)comandos.solicitacoes_recebidas,
        (unsigned long)comandos.respostas_enviadas,
        (unsigned long)comandos.solicitacoes_enviadas,
        (unsigned long)comandos.respostas_recebidas,
        (unsigned long)comandos.repeticoes,
        (unsigned long)comandos.timeouts,
        (unsigned long)comandos.quadros_invalidos, (long)comandos.ultimo_erro,
        ota.iniciado ? "true" : "false", ota.recebendo ? "true" : "false",
        (unsigned long)ota.sessao, (unsigned long)ota.tamanho_total,
        (unsigned long)ota.bytes_recebidos,
        (unsigned long)ota.quadros_recebidos, (unsigned long)ota.falhas,
        (long)ota.ultimo_erro, versao_ota);
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

static esp_err_t responder_redirecionamento_cativo(
    httpd_req_t *requisicao, httpd_err_code_t codigo)
{
    (void)codigo;
    httpd_resp_set_status(requisicao, "303 See Other");
    httpd_resp_set_hdr(requisicao, "Location", url_para_host(requisicao));
    httpd_resp_set_hdr(requisicao, "Cache-Control", "no-store, max-age=0");
    return httpd_resp_send(requisicao, "Abrindo o painel local...",
                           HTTPD_RESP_USE_STRLEN);
}

esp_err_t servidor_web_iniciar(void)
{
    httpd_handle_t servidor = NULL;
    httpd_config_t configuracao = HTTPD_DEFAULT_CONFIG();
    configuracao.server_port = PORTA_HTTP;
    // Os manipuladores montam respostas de diagnóstico e OTA na pilha.
    configuracao.stack_size = 8192;
    ESP_RETURN_ON_ERROR(httpd_start(&servidor, &configuracao), TAG, "servidor HTTP");
    ESP_RETURN_ON_ERROR(
        httpd_register_err_handler(servidor, HTTPD_404_NOT_FOUND,
                                   responder_redirecionamento_cativo),
        TAG, "redirecionamento do portal cativo");
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
    ESP_LOGI(TAG, "Painéis disponíveis em %s, %s e %s", URL_LOCAL_EQUIPE,
             URL_LOCAL_MESTRE, URL_LOCAL_DIAGNOSTICO);
    return ESP_OK;
}
