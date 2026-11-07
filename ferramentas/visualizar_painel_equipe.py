"""Prévia do painel da equipe no PC ou na rede local, com APIs simuladas."""

import argparse
import importlib.util
import json
import math
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

from visualizar_painel_visitantes import telemetria_simulada

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "gerador_equipe", ROOT / "firmware/equipe-s3/web/gerar_painel.py")
GERADOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GERADOR)

PREVIA = """
<aside class="preview-toolbar" aria-label="Controles da prévia local">
  <div><strong>Prévia local da equipe</strong><span>Todos os dados e comandos são simulados. Nenhum ESP é acessado.</span></div>
  <label>Cenário <select id="preview-scenario">
    <option value="normal">Funcionamento normal</option>
    <option value="degradado">Operação degradada</option>
    <option value="sem-mestre">Mestre sem resposta</option>
  </select></label><span id="preview-feedback" role="status"></span>
</aside>
<style>
.preview-toolbar{display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:10px 20px;padding:12px max(16px,calc((100% - 1240px)/2));background:#3a3224;color:#f7dfaa;border-bottom:1px solid #836c42;font:13px/1.4 system-ui}
.preview-toolbar strong,.preview-toolbar div span{display:block}.preview-toolbar div span{font-size:12px;margin-top:3px}
.preview-toolbar label{display:flex;align-items:center;gap:8px}.preview-toolbar select{max-width:100%;padding:8px;border:1px solid #947c51;border-radius:8px;background:#28271f;color:#fff;font:inherit}
</style>
<script>
addEventListener('DOMContentLoaded',()=>{
 const field=document.getElementById('preview-scenario'),note=document.getElementById('preview-feedback');
 fetch('/__previa/cenario').then(r=>r.json()).then(d=>{field.value=d.cenario});
 field.addEventListener('change',async()=>{
   field.disabled=true;
   try{const r=await fetch('/__previa/cenario',{method:'POST',body:field.value});if(!r.ok)throw Error();note.textContent='Cenário aplicado. Aguarde as próximas consultas.'}
   catch(e){note.textContent='Não foi possível alterar o cenário.'}
   finally{field.disabled=false}
 });
});
</script>
"""


class SimuladorEquipe:
    def __init__(self, cenario="normal"):
        self.lock = threading.RLock()
        self.inicio = time.monotonic()
        self.cenario = cenario
        self.simulado = True
        self.controle = dict(formato=1, atuacao_habilitada=False, marcha_solicitada=0,
                            parada_solicitada=True, velocidade_maxima_centesimos_kmh=0,
                            aceleracao_maxima_milesimos_ms2=0, revisao=0, alterado_em_ms=0)
        self.solicitacao = 0
        self.percurso_base = 0
        self.redes = ["Laboratório UFSM", "Rede de testes"]
        self.rede = self.redes[0]
        self.preparada = None
        self.config = dict(relatorios_pausados=False, intervalos_relatorios_ms=[1000]*4,
                           intervalo_ota_minutos=60, intervalo_telemetria_ota_ms=1000,
                           wifi_automatico=True, tentativas_wifi=3,
                           tempo_tentativa_wifi_ms=15000,
                           intervalo_tentativas_wifi_ms=3000, tempo_limite_wifi_ms=120000)
        self.clientes = {g: [f"02:00:00:00:{i:02X}:01", f"02:00:00:00:{i:02X}:02"]
                         for i, g in enumerate(("equipe", "visitantes"), 1)}
        self.limites = dict(equipe=15, visitantes=15)
        self.sync_inicio = None
        self.sync_id = 0
        self.ota_inicio = None
        self.ota_oferta_inicio = None
        self.ota_fase = "atual"
        self.ota_alvos = set()
        self.ota_atualizados = set()

    def tempo(self):
        return time.monotonic() - self.inicio

    def telemetria(self):
        d = telemetria_simulada(self.inicio)
        d.update(simulado=self.simulado, valido=self.cenario != "sem-mestre",
                 idade_ms=10 if self.cenario != "sem-mestre" else 5000,
                 sequencia=int(self.tempo()*4) % 65536, tempo_mestre_ms=int(self.tempo()*1000),
                 estado_mpu=7, temperatura_mpu_centesimos_c=3250, marcha=2,
                 taxa_pacotes_centesimos_hz=400, taxa_perda_centesimos_percentual=0,
                 falhas_crc=0, pacotes_validos=int(self.tempo()*4))
        d["percurso_metros"] = max(0, d["percurso_metros"] - self.percurso_base)
        return d

    def diagnostico(self, grupo):
        if grupo in self.clientes:
            self.wifi_clientes(grupo)
        phase = self.tempo()/8 + len(grupo)
        cpu = round(220 + 70*math.sin(phase))
        heap = round(190000 + 10000*math.sin(phase/2))
        return dict(ok=True, valido=True, idade_ms=10, versao_firmware="0.0.0-previa",
                    firmware="0.0.0-previa", tempo_ativo_ms=int(self.tempo()*1000),
                    cpu_valida=True, cpu_total_decimos_percentual=cpu,
                    cpu_nucleo_0_decimos_percentual=cpu+30, cpu_nucleo_1_decimos_percentual=cpu-30,
                    temperatura_valida=True, temperatura_centesimos_c=round(4200+170*math.sin(phase)),
                    heap_livre=heap, heap_interno_livre=heap, heap_interno_total=327680,
                    heap_interno_minimo=171000, heap_interno_maior_bloco=120000,
                    psram_disponivel=True, psram_total=8388608, psram_livre=7000000,
                    tarefas_ativas=14, motivo_reinicio=1, motivo_ultimo_reset=1,
                    wifi_ssid="NEXUS-"+grupo, clientes_wifi=len(self.clientes.get(grupo, [])),
                    uart_recepcoes_ok=int(self.tempo()*4), taxa_pacotes_centesimos_hz=400,
                    taxa_perda_centesimos_percentual=0)

    def wifi_clientes(self, grupo):
        limit = self.limites[grupo]
        if limit and self.tempo()+30 >= limit*60:
            self.clientes[grupo].clear()
        clients = [dict(mac=mac, conectado_ms=int(self.tempo()*1000)+30000)
                   for mac in self.clientes[grupo]]
        return dict(ok=True, clientes=clients, quantidade=len(clients),
                    quantidade_total=len(clients), limite_sessao_minutos=self.limites[grupo])

    def sincronizacao(self):
        elapsed = time.monotonic()-self.sync_inicio if self.sync_inicio is not None else 0
        active = self.sync_inicio is not None and elapsed < 12
        done = min(3, int(elapsed/4)) if self.sync_inicio is not None else 0
        progress = (elapsed % 4)/4 if active else 0
        return dict(iniciado=True, cartao_disponivel=True, em_andamento=active,
                    aguardando_confirmacao=active and progress > .85,
                    sincronizacoes_iniciadas=self.sync_id,
                    sincronizacoes_finalizadas=self.sync_id-int(active),
                    ultimo_resultado_sincronizacao=0, arquivos_pendentes=3-done,
                    arquivos_upload_total=3 if self.sync_id else 0,
                    arquivos_upload_concluidos=done,
                    arquivo_atual_bytes_enviados=int(65536*progress),
                    arquivo_atual_bytes_total=65536 if active else 0,
                    arquivo_em_upload=f"telemetria_demo_{done+1}.csv" if active else "",
                    velocidade_upload_bytes_s=16384 if active else 0,
                    tempo_restante_upload_s=max(0, int(12-elapsed)) if active else 0,
                    arquivos_ultima_sincronizacao=3 if done == 3 else 0,
                    bytes_ultima_sincronizacao=196608 if done == 3 else 0,
                    duracao_ultima_sincronizacao_ms=12000 if done == 3 else 0)

    def ota(self):
        elapsed = time.monotonic()-self.ota_inicio if self.ota_inicio is not None else 0
        offer_elapsed = time.monotonic()-self.ota_oferta_inicio if self.ota_oferta_inicio is not None else 0
        if self.ota_fase == "verificando" and elapsed >= 2:
            self.ota_fase = "atual" if len(self.ota_atualizados) == 3 else "aguardando"
        if self.ota_fase == "aguardando" and offer_elapsed >= 60:
            self.ota_fase = "cancelado"
        if self.ota_fase == "transferindo" and elapsed >= 10:
            self.ota_fase = "concluido"
            self.ota_atualizados.update(self.ota_alvos)
        master = dict(atual=4, verificando=3, aguardando=5, transferindo=6, concluido=7, cancelado=8)
        satellite = dict(atual=3, verificando=3, aguardando=4, transferindo=5, concluido=7, cancelado=8)
        targets = {}
        for name in ("mestre", "equipe", "visitantes"):
            phase = self.ota_fase
            if phase in ("aguardando", "cancelado") and name in self.ota_atualizados:
                phase = "atual"
            if phase in ("transferindo", "concluido") and name not in self.ota_alvos:
                phase = "atual" if name in self.ota_atualizados else "cancelado" if offer_elapsed >= 60 else "aguardando"
            targets[name] = dict(estado=(master if name == "mestre" else satellite)[phase],
                                 progresso_percentual=min(100, int(elapsed*10)) if phase in ("transferindo", "concluido") else 0,
                                 versao_atual="0.0.1-previa" if name in self.ota_atualizados else "0.0.0-previa",
                                 versao_disponivel="0.0.1-previa" if phase != "atual" or name in self.ota_atualizados else "0.0.0-previa")
        target = 1 if len(self.ota_alvos)>1 else next((i for i,n in enumerate(("mestre","equipe","visitantes"),2) if n in self.ota_alvos),0)
        return dict(estado=master[self.ota_fase], alvo=target, falhas=0, **targets)

    def consulta(self, nome, parametros):
        if self.cenario == "sem-mestre":
            raise ConnectionError("Cenário de prévia: mestre sem resposta.")
        if nome in ("ping", "satelite_equipe_ping", "satelite_visitantes_ping"):
            return dict(tempo_ativo_ms=int(self.tempo()*1000), rtt_ms=18 if nome == "ping" else 42)
        if nome in ("versao", "satelite_equipe_versao", "satelite_visitantes_versao"):
            return dict(firmware="0.0.0-previa", capacidades=16383)
        if nome == "controle_remoto":
            return dict(self.controle)
        if nome == "telemetria":
            return self.telemetria()
        if nome == "uart":
            return dict(recepcoes_ok=int(self.tempo()*4), falhas_crc=0, pacotes_perdidos=0,
                        equipe_online=True, visitantes_online=True)
        if nome in ("diagnostico_mestre", "satelite_visitantes_diagnostico"):
            return self.diagnostico("mestre" if nome == "diagnostico_mestre" else "visitantes")
        if nome == "saude":
            return dict(estado=2 if self.cenario == "degradado" else 0,
                        causas_ativas=8 if self.cenario == "degradado" else 0)
        if nome == "wifi":
            return dict(conectado=bool(self.rede), radio_ativo=True, rede=self.rede, rssi_dbm=-52, canal=6)
        if nome == "configuracao":
            return dict(self.config, quantidade_redes_wifi=len(self.redes))
        if nome == "simulador":
            return dict(ativo=self.simulado, padrao_firmware=True, amostras_geradas=int(self.tempo()*4))
        if nome == "sincronizacao":
            return self.sincronizacao()
        if nome == "ota":
            return self.ota()
        if nome == "rede_wifi_salva":
            index = int(parametros.get("indice", ["0"])[0])
            if not 1 <= index <= len(self.redes):
                raise ValueError("Selecione uma rede salva.")
            return dict(ssid=self.redes[index-1])
        if nome == "satelite_visitantes_clientes":
            return self.wifi_clientes("visitantes")
        raise ValueError("Consulta não implementada na prévia: "+nome)

    def wifi_acao(self, grupo, acao, valor):
        if acao == "limite":
            minutes = int(valor)
            if not 0 <= minutes <= 1440:
                raise ValueError("Tempo fora da faixa de 0 a 1440 minutos.")
            self.limites[grupo] = minutes
        elif acao == "kick":
            if valor not in self.clientes[grupo]:
                raise ValueError("Dispositivo não encontrado na lista simulada.")
            self.clientes[grupo].remove(valor)
        else:
            raise ValueError("Ação Wi-Fi desconhecida.")
        return {}

    def comando(self, nome, corpo):
        if self.cenario == "sem-mestre":
            raise ConnectionError("Cenário de prévia: mestre sem resposta.")
        lines = corpo.splitlines()
        fields = {"definir_marcha": ("marcha_solicitada", 2),
                  "velocidade_maxima": ("velocidade_maxima_centesimos_kmh", 3500),
                  "aceleracao_maxima": ("aceleracao_maxima_milesimos_ms2", 5000),
                  "parada_remota": ("parada_solicitada", 1)}
        if nome in fields:
            field, maximum = fields[nome]
            if not corpo.isascii() or not corpo.isdecimal() or int(corpo) > maximum:
                raise ValueError("Valor de preparação inválido.")
            value = int(corpo)
            c = self.controle
            if nome == "definir_marcha" and value and (c["parada_solicitada"] or
                    not c["velocidade_maxima_centesimos_kmh"] or not c["aceleracao_maxima_milesimos_ms2"] or
                    c["marcha_solicitada"] not in (0, value)):
                raise ValueError("Defina limites, libere a parada e passe por neutro.")
            c[field] = bool(value) if nome == "parada_remota" else value
            if c["parada_solicitada"] or not c["velocidade_maxima_centesimos_kmh"] or not c["aceleracao_maxima_milesimos_ms2"]:
                c["marcha_solicitada"] = 0
            c["revisao"] += 1
            c["alterado_em_ms"] = int(self.tempo()*1000)
            return {}
        protected = {"wifi_alterar_principal": 1, "wifi_restaurar_principal": 1,
                     "wifi_automatico": 2, "wifi_politica": 5}
        if nome in protected and (len(lines) != protected[nome] or not lines[-1].strip()):
            raise ValueError("Informe uma permissão fictícia para testar a ação protegida.")
        if nome == "sincronizar_google":
            if self.sincronizacao()["em_andamento"]:
                raise ValueError("Sincronização simulada já em andamento.")
            self.sync_id += 1
            self.sync_inicio = time.monotonic()
            return dict(sincronizacao_id=self.sync_id)
        if nome == "verificar_ota":
            self.ota_fase, self.ota_inicio, self.ota_alvos = "verificando", time.monotonic(), set()
            self.ota_oferta_inicio = self.ota_inicio
        elif nome.startswith("autorizar_"):
            status = self.ota()
            target = nome.removeprefix("autorizar_")
            requested = {"mestre","equipe","visitantes"} if target == "todos" else {target}
            pending = {name for name in ("mestre","equipe","visitantes")
                       if status[name]["estado"] == (5 if name == "mestre" else 4)}
            if self.ota_fase not in ("aguardando", "concluido") or not requested & pending:
                raise ValueError("Verifique as atualizações antes de autorizar.")
            self.ota_alvos = requested & pending
            self.ota_fase, self.ota_inicio = "transferindo", time.monotonic()
        elif nome == "cancelar_ota":
            self.ota_fase = "cancelado"
        elif nome == "simulador_telemetria":
            if corpo not in ("0","1"):
                raise ValueError("Fonte inválida.")
            self.simulado = corpo == "1"
        elif nome == "zerar_percurso":
            self.percurso_base += self.telemetria()["percurso_metros"]
        elif nome == "alternar_relatorios":
            self.config["relatorios_pausados"] = not self.config["relatorios_pausados"]
        elif nome == "resumo_terminal":
            pass
        elif nome == "intervalo_relatorio":
            group, interval = map(int, lines)
            if not 0 <= group <= 3 or not 0 <= interval <= 4294967295:
                raise ValueError("Intervalo ou grupo inválido.")
            self.config["intervalos_relatorios_ms"][group] = interval
        elif nome in ("intervalo_ota", "intervalo_telemetria_ota"):
            value = int(corpo)
            minimum, maximum = (1,10080) if nome == "intervalo_ota" else (500,10000)
            if not minimum <= value <= maximum:
                raise ValueError("Intervalo fora da faixa.")
            self.config["intervalo_ota_minutos" if nome == "intervalo_ota" else "intervalo_telemetria_ota_ms"] = value
        elif nome == "wifi_preparar":
            if len(lines) != 2 or not 1 <= len(lines[0].encode()) <= 32 or not 8 <= len(lines[1].encode()) <= 63:
                raise ValueError("Nome ou senha de teste inválidos.")
            self.preparada = lines[0]  # Senha descartada; a prévia não conecta a redes.
        elif nome in ("wifi_conectar_preparada","wifi_salvar_preparada","wifi_alterar_principal"):
            if not self.preparada:
                raise ValueError("Prepare uma rede primeiro.")
            if nome == "wifi_salvar_preparada":
                if self.preparada not in self.redes:
                    if len(self.redes) >= 5:
                        raise ValueError("A lista simulada comporta cinco redes.")
                    self.redes.append(self.preparada)
            else:
                self.rede = self.preparada
        elif nome in ("wifi_conectar_salva","wifi_excluir_salva"):
            index = int(corpo)
            if not 1 <= index <= len(self.redes):
                raise ValueError("Rede salva inexistente.")
            if nome == "wifi_conectar_salva":
                self.rede = self.redes[index-1]
            else:
                self.redes.pop(index-1)
        elif nome == "wifi_restaurar_principal":
            self.rede = "Laboratório UFSM"
        elif nome == "wifi_automatico":
            if not lines or lines[0] not in ("0", "1"):
                raise ValueError("Modo de conexão inválido.")
            self.config["wifi_automatico"] = lines[0] == "1"
        elif nome == "wifi_politica":
            values = [int(v) for v in lines[:4]]
            if len(values) != 4 or not (1 <= values[0] <= 10 and 2000 <= values[1] <= 30000 and 100 <= values[2] <= 5000 and 5000 <= values[3] <= 180000):
                raise ValueError("Política fora da faixa da prévia.")
            self.config.update(zip(("tentativas_wifi","tempo_tentativa_wifi_ms","intervalo_tentativas_wifi_ms","tempo_limite_wifi_ms"),values))
        elif nome in ("satelite_visitantes_limite","satelite_visitantes_kick"):
            return self.wifi_acao("visitantes", nome.rsplit("_",1)[1], corpo)
        else:
            raise ValueError("Comando não implementado na prévia: "+nome)
        return {}


def criar_servidor(endereco, porta, simulador):
    class Handler(BaseHTTPRequestHandler):
        def responder(self, status, data, html=False):
            body = data.encode("utf-8") if html else json.dumps(data, ensure_ascii=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "text/html; charset=utf-8" if html else "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def executar(self, post=False):
            url = urlsplit(self.path)
            query = parse_qs(url.query)
            try:
                size = int(self.headers.get("Content-Length", "0"))
                if size < 0 or size > 4096:
                    raise ValueError("Corpo de requisição inválido.")
                body = self.rfile.read(size).decode("utf-8") if post else ""
                with simulador.lock:
                    if not post and url.path in ("/", "/index.html", "/painel", "/diagnostico", "/mestre"):
                        self.responder(200, GERADOR.montar_html().replace("<body>", "<body>"+PREVIA, 1), True)
                        return
                    if url.path == "/__previa/cenario":
                        if post:
                            if body not in ("normal", "degradado", "sem-mestre"):
                                raise ValueError("Cenário inválido.")
                            simulador.cenario = body
                        result = dict(cenario=simulador.cenario)
                    elif not post and url.path == "/api/telemetria":
                        result = simulador.telemetria()
                    elif not post and url.path == "/api/diagnostico":
                        result = simulador.diagnostico("equipe")
                    elif not post and url.path == "/api/wifi/clientes":
                        result = simulador.wifi_clientes("equipe")
                    elif post and url.path in ("/api/wifi/limite", "/api/wifi/desconectar"):
                        data = json.loads(body)
                        action = "limite" if url.path.endswith("limite") else "kick"
                        result = simulador.wifi_acao("equipe", action, data["minutos" if action == "limite" else "mac"])
                    elif url.path == "/api/mestre":
                        name = query.get("consulta", [""])[0]
                        simulador.solicitacao += 1
                        result = simulador.comando(name, body) if post else simulador.consulta(name, query)
                        result.update(solicitacao=simulador.solicitacao, duracao_ms=24, tentativas=1)
                    else:
                        self.responder(404, dict(ok=False, erro="Rota não disponível na prévia."))
                        return
                    self.responder(200, dict(ok=True, **result) if "ok" not in result else result)
            except ConnectionError as error:
                self.responder(503, dict(ok=False, erro=str(error)))
            except (ValueError, KeyError, IndexError, TypeError):
                self.responder(400, dict(ok=False, erro="Solicitação inválida ou operação indisponível neste estado da prévia."))

        def do_GET(self):
            self.executar()

        def do_POST(self):
            self.executar(True)

        def log_message(self, format, *args):
            pass  # Evita milhares de linhas de polling; nenhum corpo é registrado.

    return ThreadingHTTPServer((endereco, porta), Handler)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--porta", type=int, default=8766)
    parser.add_argument("--rede", action="store_true", help="permitir acesso pelo celular na mesma rede")
    parser.add_argument("--cenario", choices=("normal","degradado","sem-mestre"), default="normal")
    args = parser.parse_args()
    if not 1 <= args.porta <= 65535:
        parser.error("A porta deve estar entre 1 e 65535.")
    with criar_servidor("0.0.0.0" if args.rede else "127.0.0.1", args.porta, SimuladorEquipe(args.cenario)) as servidor:
        print(f"Equipe: http://127.0.0.1:{args.porta}/", flush=True)
        if args.rede:
            print(f"No celular: http://<IP_IPV4_DO_PC>:{args.porta}/", flush=True)
        print("Prévia simulada. Ctrl+C encerra.", flush=True)
        try:
            servidor.serve_forever()
        except KeyboardInterrupt:
            print("\nPrévia encerrada.")


if __name__ == "__main__":
    main()
