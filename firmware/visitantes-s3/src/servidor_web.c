// Publica a página de telemetria dos visitantes e seus dados em tempo real.
#include "servidor_web.h"

#include <stdio.h>
#include <string.h>

#include "configuracao.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "receptor_uart.h"

static const char *TAG = "web_visitantes";

static const char pagina_html[] =
    "<!doctype html><html lang='pt-BR'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1,viewport-fit=cover'>"
    "<meta name='theme-color' content='#06131d'><title>Telemetria do Veículo | UFSM-CS</title><style>"
    ":root{--fundo:#06131d;--painel:#0b2030;--linha:#19445a;--texto:#edf7fb;--suave:#8ca9b9;--ciano:#21d9f3;--azul:#3195ff;--verde:#2ce6a0;--amarelo:#ffb34e;--vermelho:#ff5570;--sombra:0 20px 50px #0006}"
    "*{box-sizing:border-box}html{scroll-behavior:smooth}body{margin:0;background:#06131d;color:var(--texto);font:15px/1.5 'Segoe UI Variable',system-ui,-apple-system,'Segoe UI',sans-serif;min-height:100vh}"
    "body:before{content:'';position:fixed;inset:0;pointer-events:none;background:radial-gradient(circle at 76% -10%,#123d4c 0,transparent 34rem),repeating-linear-gradient(118deg,#ffffff02 0,#ffffff02 1px,transparent 1px,transparent 9px);opacity:.9}body:after{content:'';position:fixed;left:50%;top:0;bottom:0;border-left:1px dashed #55dff00b;pointer-events:none}"
    "a{color:inherit;text-decoration:none}h1,h2,h3{font-family:'Segoe UI Variable',system-ui,sans-serif;font-weight:670;line-height:1.08}.pagina{width:min(1220px,100%);margin:auto;padding:28px 28px 108px;position:relative}.topo{display:flex;align-items:center;justify-content:space-between;gap:18px;margin-bottom:32px;padding-bottom:20px;border-bottom:1px solid #16394a}.marca{display:flex;align-items:center;gap:14px}.logo{width:52px;height:52px;border:1px solid #2ad8ed88;border-radius:14px;display:grid;place-items:center;background:#092535;box-shadow:inset 0 0 22px #20d8f31a}.logo svg{width:31px;color:var(--ciano)}"
    ".marca h1{font-size:clamp(1.02rem,2vw,1.38rem);letter-spacing:.075em;margin:0}.marca p{color:var(--suave);margin:4px 0 0;font-size:.82rem}.ao-vivo{display:flex;align-items:center;gap:9px;padding:9px 13px;border:1px solid #276079;border-radius:999px;background:#092231;color:#abc2cd;font-size:.78rem;font-weight:700}.ao-vivo i{width:8px;height:8px;border-radius:50%;background:var(--amarelo);box-shadow:0 0 12px currentColor}.ao-vivo.ok{color:var(--verde);border-color:#27856566}.ao-vivo.ok i{background:var(--verde);animation:pulso 1.8s infinite}.ao-vivo.erro{color:#ff98a9;border-color:#a43c5066}.ao-vivo.erro i{background:var(--vermelho)}"
    "@keyframes pulso{50%{box-shadow:0 0 0 7px #2ce6a000}}@keyframes entrada{from{opacity:0;transform:translateY(10px)}to{opacity:1;transform:none}}.destaque{animation:entrada .55s ease-out}.secao{scroll-margin-top:24px;margin-top:58px}.secao-titulo{display:flex;align-items:end;justify-content:space-between;gap:16px;margin-bottom:18px;padding-left:15px;border-left:3px solid var(--ciano)}.secao-titulo h2{font-size:clamp(1.35rem,3vw,2rem);margin:0}.secao-titulo p{margin:5px 0 0;color:var(--suave)}.rotulo{color:var(--suave);font-size:.76rem;font-weight:650;letter-spacing:.01em}.unidade{font-size:.52em;color:var(--ciano);font-weight:650}.cartao{background:#091d2aee;border:1px solid var(--linha);border-radius:17px;box-shadow:0 16px 42px #0004;overflow:hidden;position:relative}.cartao:after{content:'';position:absolute;inset:0;pointer-events:none;border-top:1px solid #ffffff08}"
    ".destaque{display:grid;grid-template-columns:minmax(360px,1.32fr) minmax(300px,.68fr);gap:18px}.velocidade-card{min-height:500px;padding:30px;display:flex;flex-direction:column;align-items:center;background:radial-gradient(circle at 50% 49%,#0d2c3f 0,#091c29 46%,#071721 75%);border-color:#23607b}.chamada{align-self:stretch;display:flex;justify-content:space-between;align-items:start}.chamada h2{font-size:clamp(1.55rem,4vw,2.55rem);margin:3px 0 7px}.chamada p{margin:0;color:var(--suave);max-width:48ch}.origem{font-size:.72rem;color:#b0c5ce;padding:6px 9px;border-left:2px solid var(--ciano);background:#0d2b3a}"
    ".mostrador{width:min(100%,370px);aspect-ratio:1;position:relative;margin:-3px 0 -13px}.mostrador svg{width:100%;height:100%;transform:rotate(135deg);filter:drop-shadow(0 0 9px #269ef744)}.arco{fill:none;stroke:#17394e;stroke-width:16;stroke-linecap:round;stroke-dasharray:75 100}.arco-valor{fill:none;stroke:url(#gradiente);stroke-width:16;stroke-linecap:round;stroke-dasharray:0 100;transition:stroke-dasharray .35s ease}.numero-velocidade{position:absolute;inset:0;display:grid;place-content:center;text-align:center;padding-top:14px}.numero-velocidade strong{font-family:'Bahnschrift',ui-monospace,monospace;font-size:clamp(4.8rem,11vw,7.6rem);font-weight:620;letter-spacing:-.08em;line-height:.86;color:#f4fbff;text-shadow:0 0 34px #218cff66;font-variant-numeric:tabular-nums}.numero-velocidade span{font-size:1.05rem;font-weight:650;margin-top:13px;color:#b5cad3}.faixa{position:absolute;inset:12%;border-radius:50%;border:1px solid #24749c55;box-shadow:inset 0 0 34px #0008}.escala span{position:absolute;color:#668898;font:600 .68rem ui-monospace,monospace}.escala span:nth-child(1){left:10%;bottom:20%}.escala span:nth-child(2){left:15%;top:23%}.escala span:nth-child(3){right:15%;top:23%}.escala span:nth-child(4){right:8%;bottom:20%}"
    ".controles{width:100%;display:grid;grid-template-columns:1fr 92px 1fr;gap:12px;align-items:stretch}.pedal,.marcha{border:1px solid #1b5268;border-radius:17px;background:#0a1d2a;padding:14px}.pedal .linha{display:flex;justify-content:space-between;align-items:center;margin-bottom:10px}.pedal b{font-size:1.1rem;color:var(--verde)}.pedal.freio b{color:var(--amarelo)}.barra{height:8px;background:#153449;border-radius:99px;overflow:hidden}.barra i{display:block;width:0;height:100%;background:linear-gradient(90deg,#1bbbd7,var(--verde));border-radius:inherit;transition:width .35s}.freio .barra i{background:linear-gradient(90deg,#ffcf68,var(--amarelo))}.marcha{display:grid;place-content:center;text-align:center;border-color:#1fc8dd88}.marcha span{font-size:.65rem;color:var(--suave);letter-spacing:.12em}.marcha b{font-size:2.3rem;color:var(--ciano);line-height:1.05}"
    ".coluna{display:grid;gap:18px}.atitude{padding:24px;min-height:285px}.cabecalho-cartao{display:flex;align-items:center;justify-content:space-between;gap:12px}.icone{width:38px;height:38px;border-radius:12px;background:#0a3d4d;display:grid;place-items:center;color:var(--ciano);font-size:1.2rem}.atitude-visual{height:130px;display:grid;place-items:center;perspective:450px}.carro-inclinacao{width:78px;height:124px;border-radius:30px 30px 22px 22px;border:3px solid var(--ciano);background:linear-gradient(#2ccde9,#116786);box-shadow:0 0 28px #22d9f344;position:relative;transition:transform .35s ease}.carro-inclinacao:before{content:'';position:absolute;left:12px;right:12px;top:28px;height:44px;border-radius:15px;background:linear-gradient(#c3f5ffcc,#368aa9aa);border:1px solid #d8f9ff88}.carro-inclinacao:after{content:'';position:absolute;left:20px;bottom:10px;width:12px;height:12px;border-radius:50%;background:var(--vermelho);box-shadow:26px 0 var(--vermelho)}.eixos{display:grid;grid-template-columns:1fr 1fr;gap:9px}.eixos div{background:#081923;border:1px solid #183e51;border-radius:12px;padding:11px}.eixos b{display:block;font-size:1.15rem;margin-top:4px;font-variant-numeric:tabular-nums}"
    ".resumo{padding:22px;display:grid;grid-template-columns:repeat(2,1fr);gap:12px}.resumo>div{padding:10px}.resumo b{display:block;font-size:1.45rem;margin-top:5px;font-variant-numeric:tabular-nums}.resumo .largo{grid-column:1/-1;border-top:1px solid #173b4d;padding-top:18px}.coordenada{font-size:.92rem!important;color:#bfd2db;word-break:break-word}"
    ".grade-dinamica{display:grid;grid-template-columns:repeat(4,1fr);gap:1px;padding:1px;background:#1a4356;border:1px solid #1a4356;border-radius:17px;overflow:hidden}.grade-dinamica .metrica{padding:21px;min-height:132px;border:0;border-radius:0;box-shadow:none;background:#081c29}.metrica b{display:block;font-family:'Bahnschrift',ui-monospace,monospace;font-size:clamp(1.55rem,3vw,2.15rem);font-weight:580;margin-top:19px;font-variant-numeric:tabular-nums}.metrica.acento-verde{box-shadow:inset 0 3px var(--verde)}.metrica.acento-amarelo{box-shadow:inset 0 3px var(--amarelo)}.vetores{grid-column:span 2}.vetor-grade{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-top:16px}.vetor-grade div{background:#061721;border-left:2px solid #1d566e;padding:10px;text-align:center}.vetor-grade b{font-size:1rem;margin:5px 0 0}.nota{color:#7795a5;font-size:.74rem;margin:12px 0 0}"
    ".energia{display:grid;grid-template-columns:1.05fr .95fr;gap:18px}.bateria{padding:28px}.carga-linha{display:flex;align-items:end;justify-content:space-between;margin:24px 0 15px}.carga-linha b{font-size:clamp(3.8rem,8vw,6rem);line-height:.8;color:var(--verde);font-variant-numeric:tabular-nums}.carga-linha span{text-align:right;color:var(--suave)}.carga-linha strong{display:block;color:var(--texto);font-size:1.5rem;margin-bottom:4px}.barra-carga{height:17px;background:#173a50;border-radius:99px;overflow:hidden;box-shadow:inset 0 2px 5px #0006}.barra-carga i{display:block;width:0;height:100%;border-radius:inherit;background:linear-gradient(90deg,#20bdec,var(--verde));box-shadow:0 0 16px #2ce6a077;transition:width .45s}.energia-grade{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;margin-top:24px;padding-top:22px;border-top:1px solid #184358}.energia-grade b{display:block;font-size:1.35rem;margin-top:7px;font-variant-numeric:tabular-nums}"
    ".celulas{padding:28px}.celula-grade{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-top:22px}.celula{padding:18px;background:#081b28;border:1px solid #1b6a5999;border-radius:17px}.celula b{display:block;font-size:1.65rem;color:var(--verde);margin-top:12px;font-variant-numeric:tabular-nums}.equilibrio{display:flex;justify-content:space-between;margin-top:18px;padding-top:18px;border-top:1px solid #174255}.equilibrio b{color:var(--verde)}"
    ".proximidade-layout{display:grid;grid-template-columns:minmax(380px,1fr) .65fr;gap:18px}.radar{padding:28px;min-height:520px}.radar-grade{width:min(100%,500px);margin:24px auto 0;display:grid;grid-template-columns:1fr 1.05fr 1fr;grid-template-rows:112px 170px 112px;gap:7px;align-items:center}.sensor{justify-self:center;text-align:center;min-width:94px;padding:9px 11px;border:1px solid #217558;border-radius:999px;background:#0b2929;color:var(--verde);font-weight:750;font-variant-numeric:tabular-nums;box-shadow:0 0 18px #2ce6a014}.sensor.atencao{color:var(--amarelo);border-color:#a46c2d;background:#302417}.sensor.critico{color:#ff7085;border-color:#a94054;background:#321824}.sensor.sem-dado{color:#6e8997;border-color:#294555;background:#0b1b25}.s0{grid-area:1/2}.s1{grid-area:1/3}.s2{grid-area:2/3}.s3{grid-area:3/3}.s4{grid-area:3/2}.s5{grid-area:3/1}.s6{grid-area:2/1}.s7{grid-area:1/1}.carro-radar{grid-area:2/2;justify-self:center;width:86px;height:146px;border-radius:34px 34px 24px 24px;background:linear-gradient(#38d9ec,#116c8d);border:3px solid #8defff;position:relative;box-shadow:0 0 34px #22d9f344}.carro-radar:before{content:'';position:absolute;inset:31px 14px 44px;border-radius:17px;background:linear-gradient(#c8f7ffdd,#4d98b5cc)}.carro-radar:after{content:'';position:absolute;left:-8px;top:31px;width:10px;height:38px;border-radius:8px;background:#02090d;box-shadow:92px 0 #02090d,0 58px #02090d,92px 58px #02090d}.legenda{display:flex;gap:18px;flex-wrap:wrap;margin-top:20px;color:var(--suave);font-size:.78rem}.legenda i{display:inline-block;width:9px;height:9px;border-radius:50%;margin-right:6px}.mais-proximo{padding:28px;display:flex;flex-direction:column;justify-content:center}.mais-proximo strong{font-size:clamp(2rem,5vw,3.5rem);margin:10px 0 6px}.mais-proximo .distancia-destaque{font-size:1.45rem;color:var(--verde)}.mais-proximo p{color:var(--suave);line-height:1.6}.orientacao{margin-top:26px;padding-top:22px;border-top:1px solid #194357}.orientacao b{display:block;font-size:1.6rem;color:var(--ciano);margin-top:7px}"
    ".historico{padding:26px}.historico-topo{display:flex;justify-content:space-between;align-items:center;gap:16px;flex-wrap:wrap}.series{display:flex;gap:8px;flex-wrap:wrap}.serie{border:1px solid #245169;border-radius:999px;padding:8px 11px;color:#a7bfca;background:#0a1b26;font-size:.72rem}.serie i{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px}.grafico-wrap{height:300px;margin-top:22px;position:relative}.grafico-wrap canvas{width:100%;height:100%;display:block}.grafico-rodape{display:flex;justify-content:space-between;color:#6f8e9e;font-size:.7rem;margin-top:5px}"
    ".navegacao{position:fixed;z-index:10;left:50%;bottom:max(14px,env(safe-area-inset-bottom));transform:translateX(-50%);display:flex;gap:3px;padding:6px;border:1px solid #20516a;background:#071721f2;backdrop-filter:blur(16px);border-radius:15px;box-shadow:0 15px 45px #000a}.navegacao a{display:flex;align-items:center;gap:7px;padding:10px 14px;border-radius:10px;color:#91adba;font-size:.74rem;font-weight:650;font-style:normal;white-space:nowrap;outline:none}.navegacao a em{font-style:normal}.navegacao a:hover,.navegacao a:focus-visible{background:#103346;color:var(--texto);box-shadow:inset 0 -2px var(--ciano)}.navegacao span{font-size:1rem}"
    "@media(max-width:860px){.pagina{padding:20px 16px 102px}.topo{margin-bottom:20px}.destaque,.energia,.proximidade-layout{grid-template-columns:1fr}.velocidade-card{min-height:auto}.coluna{grid-template-columns:1fr 1fr}.grade-dinamica{grid-template-columns:repeat(2,1fr)}.radar{min-height:auto}.mais-proximo{min-height:230px}.navegacao a{padding:10px 12px}.navegacao a em{display:none}}"
    "@media(max-width:560px){.pagina{padding-left:13px;padding-right:13px}.marca p{display:none}.logo{width:44px;height:44px}.ao-vivo{padding:8px 10px}.destaque{gap:13px}.velocidade-card{padding:21px 16px}.chamada{padding:3px 5px}.chamada p{max-width:24ch}.origem{display:none}.mostrador{margin:-4px 0 -20px}.controles{grid-template-columns:1fr 76px 1fr;gap:7px}.pedal,.marcha{padding:11px 9px}.coluna{grid-template-columns:1fr}.resumo{grid-template-columns:repeat(2,1fr)}.grade-dinamica{gap:1px}.grade-dinamica .metrica{padding:16px;min-height:116px}.vetores{grid-column:1/-1}.energia-grade{grid-template-columns:1fr 1fr}.energia-grade>div:last-child{grid-column:1/-1}.bateria,.celulas,.radar,.mais-proximo,.historico{padding:20px}.celula{padding:14px}.radar-grade{grid-template-rows:93px 150px 93px}.sensor{min-width:76px;padding:8px 6px;font-size:.76rem}.carro-radar{transform:scale(.85)}.secao{margin-top:42px}.secao-titulo p{font-size:.78rem}.navegacao{width:calc(100% - 20px);justify-content:space-around;bottom:max(8px,env(safe-area-inset-bottom))}.navegacao a{padding:10px}.grafico-wrap{height:240px}}"
    "@media(prefers-reduced-motion:reduce){*{scroll-behavior:auto!important;animation:none!important;transition:none!important}}</style></head><body>"
    "<main class='pagina'><header class='topo'><div class='marca'><div class='logo' aria-hidden='true'><svg viewBox='0 0 48 48' fill='none'><path d='M11 28h26l-3-11H15l-4 11Z' stroke='currentColor' stroke-width='3'/><path d='M8 28v8h5m27-8v8h-5M16 36h16' stroke='currentColor' stroke-width='3' stroke-linecap='round'/><circle cx='16' cy='29' r='3' fill='currentColor'/><circle cx='32' cy='29' r='3' fill='currentColor'/><path d='m26 5-6 9h6l-3 8 9-11h-6l3-6' fill='#2ce6a0'/></svg></div><div><h1>TELEMETRIA DO VEÍCULO</h1><p>Carro elétrico · UFSM-CS</p></div></div><div id='estado' class='ao-vivo'><i></i><span>Aguardando</span></div></header>"
    "<section id='inicio' class='destaque'><article class='cartao velocidade-card'><div class='chamada'><div><div class='rotulo'>Visão geral</div><h2>Estado atual do veículo</h2><p>Uma leitura clara de movimento, controle e energia</p></div><span id='origem' class='origem'>telemetria</span></div><div class='mostrador'><svg viewBox='0 0 260 260' aria-hidden='true'><defs><linearGradient id='gradiente'><stop stop-color='#21d9f3'/><stop offset='1' stop-color='#3195ff'/></linearGradient></defs><circle class='arco' cx='130' cy='130' r='101' pathLength='100'/><circle id='arcoVelocidade' class='arco-valor' cx='130' cy='130' r='101' pathLength='100'/></svg><div class='faixa'></div><div class='escala' aria-hidden='true'><span>0</span><span>40</span><span>80</span><span>120</span></div><div class='numero-velocidade'><strong id='velocidade'>0</strong><span>km/h</span></div></div><div class='controles'><div class='pedal'><div class='linha'><span class='rotulo'>Acelerador</span><b id='acelerador'>0%</b></div><div class='barra'><i id='barraAcelerador'></i></div></div><div class='marcha'><span>Marcha</span><b id='marcha'>P</b></div><div class='pedal freio'><div class='linha'><span class='rotulo'>Freio</span><b id='freio'>0%</b></div><div class='barra'><i id='barraFreio'></i></div></div></div></article>"
    "<div class='coluna'><article class='cartao atitude'><div class='cabecalho-cartao'><div><div class='rotulo'>Atitude do veículo</div><h3>Inclinação estimada</h3></div><div class='icone'>◇</div></div><div class='atitude-visual'><div id='carroInclinacao' class='carro-inclinacao'></div></div><div class='eixos'><div><span class='rotulo'>Rolagem</span><b id='rolagem'>—</b></div><div><span class='rotulo'>Inclinação</span><b id='inclinacao'>—</b></div></div></article><article class='cartao resumo'><div><span class='rotulo'>Autonomia</span><b id='autonomiaResumo'>—</b></div><div><span class='rotulo'>Percurso</span><b id='percursoResumo'>—</b></div><div class='largo'><span class='rotulo'>Posição atual</span><b id='posicaoResumo' class='coordenada'>Aguardando coordenadas</b></div></article></div></section>"
    "<section id='dinamica' class='secao'><div class='secao-titulo'><div><h2>Dinâmica do veículo</h2><p>Movimento e resposta em tempo real</p></div></div><div class='grade-dinamica'><article class='cartao metrica acento-verde'><span class='rotulo'>Velocidade</span><b><span id='velocidadeCard'>0,0</span> <small class='unidade'>km/h</small></b></article><article class='cartao metrica'><span class='rotulo'>Aceleração longitudinal</span><b><span id='aceleracao'>0,00</span> <small class='unidade'>m/s²</small></b></article><article class='cartao metrica'><span class='rotulo'>Percurso</span><b><span id='percurso'>0,00</span> <small class='unidade'>km</small></b></article><article class='cartao metrica'><span class='rotulo'>Odômetro</span><b><span id='odometro'>0,0</span> <small class='unidade'>km</small></b></article><article class='cartao metrica acento-amarelo'><span class='rotulo'>Potência</span><b><span id='potencia'>0</span> <small class='unidade'>W</small></b></article><article class='cartao metrica'><span class='rotulo'>Temperatura do sensor</span><b><span id='temperatura'>—</span> <small class='unidade'>°C</small></b></article><article class='cartao metrica vetores'><span class='rotulo'>Aceleração nos três eixos</span><div class='vetor-grade'><div><span class='rotulo'>X</span><b id='accX'>—</b></div><div><span class='rotulo'>Y</span><b id='accY'>—</b></div><div><span class='rotulo'>Z</span><b id='accZ'>—</b></div></div><p class='nota'>A inclinação é estimada pelo acelerômetro e pode variar durante acelerações intensas.</p></article></div></section>"
    "<section id='energia' class='secao'><div class='secao-titulo'><div><h2>Energia</h2><p>Estado do conjunto de baterias</p></div></div><div class='energia'><article class='cartao bateria'><div class='cabecalho-cartao'><div><div class='rotulo'>Energia disponível</div><h3>Bateria principal</h3></div><div class='icone'>ϟ</div></div><div class='carga-linha'><b><span id='carga'>0</span><small class='unidade'>%</small></b><span><strong id='autonomia'>—</strong>autonomia estimada</span></div><div class='barra-carga'><i id='barraCarga'></i></div><div class='energia-grade'><div><span class='rotulo'>Tensão do pacote</span><b><span id='tensaoPacote'>0,00</span> <small class='unidade'>V</small></b></div><div><span class='rotulo'>Corrente</span><b><span id='corrente'>0,0</span> <small class='unidade'>A</small></b></div><div><span class='rotulo'>Potência</span><b><span id='potenciaEnergia'>0</span> <small class='unidade'>W</small></b></div></div></article><article class='cartao celulas'><div class='cabecalho-cartao'><div><div class='rotulo'>Tensão individual</div><h3>Células da bateria</h3></div><div class='icone'>▥</div></div><div class='celula-grade'><div class='celula'><span class='rotulo'>Célula 1</span><b id='celula0'>—</b></div><div class='celula'><span class='rotulo'>Célula 2</span><b id='celula1'>—</b></div><div class='celula'><span class='rotulo'>Célula 3</span><b id='celula2'>—</b></div><div class='celula'><span class='rotulo'>Célula 4</span><b id='celula3'>—</b></div></div><div class='equilibrio'><span>Diferença entre células</span><b id='diferencaCelulas'>—</b></div></article></div></section>"
    "<section id='proximidade' class='secao'><div class='secao-titulo'><div><h2>Proximidade 360°</h2><p>Oito zonas ao redor do veículo</p></div></div><div class='proximidade-layout'><article class='cartao radar'><div class='cabecalho-cartao'><div><div class='rotulo'>Entorno do carro</div><h3>Distância dos obstáculos</h3></div><div class='icone'>◎</div></div><div class='radar-grade'><div id='sensor0' class='sensor s0'>—</div><div id='sensor1' class='sensor s1'>—</div><div id='sensor2' class='sensor s2'>—</div><div id='sensor3' class='sensor s3'>—</div><div id='sensor4' class='sensor s4'>—</div><div id='sensor5' class='sensor s5'>—</div><div id='sensor6' class='sensor s6'>—</div><div id='sensor7' class='sensor s7'>—</div><div class='carro-radar' aria-label='Veículo visto de cima'></div></div><div class='legenda'><span><i style='background:var(--vermelho)'></i>Crítico</span><span><i style='background:var(--amarelo)'></i>Atenção</span><span><i style='background:var(--verde)'></i>Livre</span></div></article><article class='cartao mais-proximo'><span class='rotulo'>Obstáculo mais próximo</span><strong id='zonaProxima'>Aguardando</strong><span id='distanciaProxima' class='distancia-destaque'>—</span><p>As cores mudam conforme a distância medida em cada região do veículo.</p><div class='orientacao'><span class='rotulo'>Rumo do veículo</span><b id='rumo'>—</b></div></article></div></section>"
    "<section id='historico' class='secao'><div class='secao-titulo'><div><h2>Evolução recente</h2><p>Últimas 40 leituras recebidas</p></div></div><article class='cartao historico'><div class='historico-topo'><div><div class='rotulo'>Comparação temporal</div><h3>Movimento e energia</h3></div><div class='series'><span class='serie'><i style='background:#2ce6a0'></i>Velocidade</span><span class='serie'><i style='background:#21d9f3'></i>Aceleração</span><span class='serie'><i style='background:#aa7dff'></i>Carga</span></div></div><div class='grafico-wrap'><canvas id='grafico' aria-label='Histórico de velocidade, aceleração e carga'></canvas></div><div class='grafico-rodape'><span>mais antigo</span><span>cada série usa sua própria escala</span><span>agora</span></div></article></section></main>"
    "<nav class='navegacao' aria-label='Seções da telemetria'><a href='#inicio'><span>⌂</span><em>Início</em></a><a href='#dinamica'><span>⌁</span><em>Dinâmica</em></a><a href='#energia'><span>ϟ</span><em>Energia</em></a><a href='#proximidade'><span>◎</span><em>Proximidade</em></a><a href='#historico'><span>⌁</span><em>Histórico</em></a></nav>"
    "<script>const q=id=>document.getElementById(id),lim=(n,a,b)=>Math.max(a,Math.min(b,n)),fmt=(n,c=1)=>Number(n).toLocaleString('pt-BR',{minimumFractionDigits:c,maximumFractionDigits:c});let consultando=false,ultimaSequencia=-1;const historico={v:[],a:[],c:[]},nomesZonas=['frente','frontal direita','lado direito','traseira direita','traseira','traseira esquerda','lado esquerdo','frontal esquerda'];"
    "function texto(id,valor){q(id).textContent=valor}function preencher(id,n){q(id).style.width=lim(n,0,100)+'%'}function classeSensor(n){return !n||n>=65535?'sensor sem-dado':n<=50?'sensor critico':n<=120?'sensor atencao':'sensor'}"
    "function atualizarSensores(valores){let menor=Infinity,indice=-1;(valores||[]).forEach((n,i)=>{let e=q('sensor'+i);e.className=classeSensor(n)+' s'+i;e.textContent=n&&n<65535?n+' cm':'—';if(n>0&&n<65535&&n<menor){menor=n;indice=i}});texto('zonaProxima',indice<0?'Sem leitura':nomesZonas[indice]);texto('distanciaProxima',indice<0?'—':menor+' cm')}"
    "function angulos(d){let x=d.aceleracao_x_milesimos_g/1000,y=d.aceleracao_y_milesimos_g/1000,z=d.aceleracao_z_milesimos_g/1000,m=Math.sqrt(x*x+y*y+z*z);if(m<.15)return null;return{r:Math.atan2(y,Math.sqrt(x*x+z*z))*180/Math.PI,p:Math.atan2(-x,Math.sqrt(y*y+z*z))*180/Math.PI}}"
    "function adicionarHistorico(v,a,c,sequencia){if(sequencia===ultimaSequencia)return;ultimaSequencia=sequencia;[['v',v],['a',a],['c',c]].forEach(([k,n])=>{historico[k].push(n);if(historico[k].length>40)historico[k].shift()});desenharGrafico()}"
    "function desenharGrafico(){let c=q('grafico'),r=c.getBoundingClientRect(),dpr=Math.min(devicePixelRatio||1,2);c.width=Math.max(1,r.width*dpr);c.height=Math.max(1,r.height*dpr);let x=c.getContext('2d');x.scale(dpr,dpr);let w=r.width,h=r.height,p=18;x.clearRect(0,0,w,h);x.strokeStyle='#19445a';x.lineWidth=1;for(let i=0;i<5;i++){let y=p+(h-2*p)*i/4;x.beginPath();x.moveTo(p,y);x.lineTo(w-p,y);x.stroke()}[['v','#2ce6a0'],['a','#21d9f3'],['c','#aa7dff']].forEach(([k,cor])=>{let s=historico[k];if(s.length<2)return;let min=Math.min(...s),max=Math.max(...s);if(max-min<.01){max+=1;min-=1}x.strokeStyle=cor;x.lineWidth=2.5;x.lineJoin='round';x.beginPath();s.forEach((n,i)=>{let px=p+(w-2*p)*i/39,py=h-p-(h-2*p)*(n-min)/(max-min);i?x.lineTo(px,py):x.moveTo(px,py)});x.stroke()})}"
    "function aplicar(d){let v=d.velocidade_centesimos_kmh/100,a=d.aceleracao_milesimos_ms2/1000,c=d.carga_decimos_percentual/10,ac=d.acelerador_decimos_percentual/10,fr=d.freio_decimos_percentual/10,marchas=['P','N','D','R'];texto('velocidade',Math.round(v));texto('velocidadeCard',fmt(v));q('arcoVelocidade').style.strokeDasharray=(lim(v/120,0,1)*75)+' 100';texto('marcha',marchas[d.marcha]||'?');texto('acelerador',fmt(ac,0)+'%');texto('freio',fmt(fr,0)+'%');preencher('barraAcelerador',ac);preencher('barraFreio',fr);texto('origem',d.simulado?'dados simulados':'telemetria real');texto('autonomiaResumo',fmt(d.autonomia_decimos_km/10)+' km');texto('percursoResumo',fmt(d.percurso_metros/1000,2)+' km');texto('posicaoResumo',d.latitude_micrograus||d.longitude_micrograus?fmt(d.latitude_micrograus/1e6,6)+'°, '+fmt(d.longitude_micrograus/1e6,6)+'°':'Coordenadas indisponíveis');texto('aceleracao',fmt(a,2));texto('percurso',fmt(d.percurso_metros/1000,2));texto('odometro',fmt(d.odometro_metros/1000));texto('potencia',d.potencia_w);texto('temperatura',fmt(d.temperatura_mpu_centesimos_c/100));texto('accX',fmt(d.aceleracao_x_milesimos_g/1000,3)+' g');texto('accY',fmt(d.aceleracao_y_milesimos_g/1000,3)+' g');texto('accZ',fmt(d.aceleracao_z_milesimos_g/1000,3)+' g');texto('carga',fmt(c,0));preencher('barraCarga',c);texto('autonomia',fmt(d.autonomia_decimos_km/10)+' km');texto('tensaoPacote',fmt(d.tensao_pacote_mv/1000,2));texto('corrente',fmt(d.corrente_centesimos_a/100));texto('potenciaEnergia',d.potencia_w);let cells=d.tensoes_celulas_mv||[];cells.forEach((n,i)=>texto('celula'+i,n?fmt(n/1000,3)+' V':'—'));let validas=cells.filter(n=>n>0);texto('diferencaCelulas',validas.length?fmt(Math.max(...validas)-Math.min(...validas),0)+' mV':'—');atualizarSensores(d.distancias_cm);texto('rumo',fmt(d.rumo_decimos_grau/10,1)+'°');let ag=angulos(d);texto('rolagem',ag?fmt(ag.r,1)+'°':'—');texto('inclinacao',ag?fmt(ag.p,1)+'°':'—');q('carroInclinacao').style.transform=ag?'rotateZ('+lim(ag.r,-35,35)+'deg) rotateX('+lim(ag.p,-25,25)+'deg)':'';adicionarHistorico(v,a,c,d.sequencia)}"
    "async function atualizar(){if(consultando)return;consultando=true;try{let r=await fetch('/api/telemetria',{cache:'no-store'});if(!r.ok)throw Error();let d=await r.json(),ok=d.valido&&d.idade_ms<2000,e=q('estado');e.className='ao-vivo '+(ok?'ok':'erro');e.querySelector('span').textContent=ok?'Ao vivo':'Aguardando veículo';aplicar(d)}catch(_){let e=q('estado');e.className='ao-vivo erro';e.querySelector('span').textContent='Aguardando veículo'}finally{consultando=false}}window.addEventListener('resize',desenharGrafico);atualizar();setInterval(atualizar,250)</script></body></html>";

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
    uint32_t idade = dados.ultimo_recebimento_ms == 0
                         ? UINT32_MAX
                         : agora - dados.ultimo_recebimento_ms;
    char json[2048];
    int tamanho = snprintf(
        json, sizeof(json),
        "{\"valido\":%s,\"idade_ms\":%lu,\"sequencia\":%u,"
        "\"marcha\":%u,\"simulado\":%s,\"velocidade_centesimos_kmh\":%u,"
        "\"aceleracao_milesimos_ms2\":%d,\"acelerador_decimos_percentual\":%u,"
        "\"freio_decimos_percentual\":%u,\"tensao_pacote_mv\":%u,"
        "\"corrente_centesimos_a\":%d,\"carga_decimos_percentual\":%u,"
        "\"tensoes_celulas_mv\":[%u,%u,%u,%u],"
        "\"latitude_micrograus\":%ld,\"longitude_micrograus\":%ld,"
        "\"rumo_decimos_grau\":%u,\"distancias_cm\":[%u,%u,%u,%u,%u,%u,%u,%u],"
        "\"potencia_w\":%d,\"autonomia_decimos_km\":%u,"
        "\"percurso_metros\":%lu,\"odometro_metros\":%lu,"
        "\"aceleracao_x_milesimos_g\":%d,\"aceleracao_y_milesimos_g\":%d,"
        "\"aceleracao_z_milesimos_g\":%d,\"temperatura_mpu_centesimos_c\":%d}",
        dados.valido ? "true" : "false", (unsigned long)idade,
        (unsigned)dados.sequencia, (unsigned)dados.marcha,
        dados.simulado ? "true" : "false",
        (unsigned)dados.velocidade_centesimos_kmh,
        dados.aceleracao_milesimos_ms2,
        (unsigned)dados.acelerador_decimos_percentual,
        (unsigned)dados.freio_decimos_percentual,
        (unsigned)dados.tensao_pacote_mv, dados.corrente_centesimos_a,
        (unsigned)dados.carga_decimos_percentual,
        (unsigned)dados.tensoes_celulas_mv[0],
        (unsigned)dados.tensoes_celulas_mv[1],
        (unsigned)dados.tensoes_celulas_mv[2],
        (unsigned)dados.tensoes_celulas_mv[3],
        (long)dados.latitude_micrograus, (long)dados.longitude_micrograus,
        (unsigned)dados.rumo_decimos_grau,
        (unsigned)dados.distancias_cm[0], (unsigned)dados.distancias_cm[1],
        (unsigned)dados.distancias_cm[2], (unsigned)dados.distancias_cm[3],
        (unsigned)dados.distancias_cm[4], (unsigned)dados.distancias_cm[5],
        (unsigned)dados.distancias_cm[6], (unsigned)dados.distancias_cm[7],
        dados.potencia_w, (unsigned)dados.autonomia_decimos_km,
        (unsigned long)dados.percurso_metros,
        (unsigned long)dados.odometro_metros,
        dados.aceleracao_x_milesimos_g, dados.aceleracao_y_milesimos_g,
        dados.aceleracao_z_milesimos_g, dados.temperatura_mpu_centesimos_c);
    if (tamanho < 0 || tamanho >= (int)sizeof(json)) {
        return httpd_resp_send_err(requisicao,
                                   HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "telemetria indisponivel");
    }
    httpd_resp_set_type(requisicao, "application/json");
    httpd_resp_set_hdr(requisicao, "Cache-Control", "no-store");
    return httpd_resp_send(requisicao, json, tamanho);
}

static void tarefa_dns_cativo(void *argumento)
{
    (void)argumento;
    int soquete = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (soquete < 0) {
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in endereco = {
        .sin_family = AF_INET,
        .sin_port = htons(PORTA_DNS),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(soquete, (struct sockaddr *)&endereco, sizeof(endereco)) < 0) {
        close(soquete);
        vTaskDelete(NULL);
        return;
    }
    while (true) {
        uint8_t pacote[512];
        struct sockaddr_in cliente;
        socklen_t tamanho_cliente = sizeof(cliente);
        int tamanho = recvfrom(soquete, pacote, sizeof(pacote), 0,
                               (struct sockaddr *)&cliente, &tamanho_cliente);
        if (tamanho < 17) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        int fim_nome = 12;
        while (fim_nome < tamanho && pacote[fim_nome] != 0) {
            fim_nome += pacote[fim_nome] + 1;
        }
        if (fim_nome + 5 > tamanho) {
            continue;
        }
        int fim_pergunta = fim_nome + 5;
        pacote[2] = 0x81;
        pacote[3] = 0x80;
        pacote[6] = 0;
        pacote[7] = 1;
        pacote[8] = pacote[9] = pacote[10] = pacote[11] = 0;
        const uint8_t resposta[] = {
            0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 0, 0, 4, 192, 168, 4, 1,
        };
        if (fim_pergunta + (int)sizeof(resposta) <= (int)sizeof(pacote)) {
            memcpy(&pacote[fim_pergunta], resposta, sizeof(resposta));
            sendto(soquete, pacote, fim_pergunta + sizeof(resposta), 0,
                   (struct sockaddr *)&cliente, tamanho_cliente);
        }
    }
}

esp_err_t servidor_web_iniciar(void)
{
    httpd_handle_t servidor = NULL;
    httpd_config_t configuracao = HTTPD_DEFAULT_CONFIG();
    configuracao.server_port = PORTA_HTTP;
    configuracao.stack_size = 8192;
    esp_err_t erro = httpd_start(&servidor, &configuracao);
    if (erro != ESP_OK) {
        return erro;
    }
    const httpd_uri_t rota_painel = {
        .uri = "/", .method = HTTP_GET, .handler = responder_painel,
    };
    const httpd_uri_t rota_api = {
        .uri = "/api/telemetria",
        .method = HTTP_GET,
        .handler = responder_telemetria,
    };
    erro = httpd_register_uri_handler(servidor, &rota_painel);
    if (erro == ESP_OK) {
        erro = httpd_register_uri_handler(servidor, &rota_api);
    }
    if (erro != ESP_OK ||
        xTaskCreate(tarefa_dns_cativo, "dns_visitantes", 4096, NULL, 4,
                    NULL) != pdPASS) {
        if (erro == ESP_OK) {
            erro = ESP_ERR_NO_MEM;
        }
        httpd_stop(servidor);
        ESP_LOGE(TAG, "Inicialização web incompleta; servidor encerrado: %s",
                 esp_err_to_name(erro));
        return erro;
    }
    ESP_LOGI(TAG, "Painel de visitantes em http://%s", IP_PAINEL);
    return ESP_OK;
}
