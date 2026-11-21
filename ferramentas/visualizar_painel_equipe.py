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

from previa_painel import adicionar_opcoes_acesso, selecionar_endereco, servir_previa
from visualizar_painel_visitantes import telemetria_simulada

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "gerador_equipe", ROOT / "firmware/equipe-s3/web/gerar_painel.py")
GERADOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GERADOR)

CAPACIDADES_MESTRE = 16383 | (1 << 14) | (1 << 15) | (1 << 16) | (1 << 17) | (1 << 18)
TODOS_SENSORES = 0b11111
QUANTIDADE_FONTES_LOG = 18
PARADA_AUTOMATICA_S = 30
PARADA_MINIMA_S = 5
ERROS_I2C_PREVIA = 4
TAREFAS_POR_PAGINA = 3
LIMITE_TAREFAS = 48
POLITICA_WIFI_PADRAO = dict(
    tentativas_wifi=3,
    tempo_tentativa_wifi_ms=8000,
    intervalo_tentativas_wifi_ms=750,
    tempo_limite_wifi_ms=30000,
)
TAREFAS_PREVIA = (
    ("telemetria", 5120, 1936, 11, 1, 2, 143),
    ("enlace_equipe", 6144, 1640, 10, 1, 2, 83),
    ("enlace_visitantes", 5120, 1840, 9, 1, 2, 54),
    ("aquisicao", 6144, 2368, 12, 0, 2, 121),
    ("supervisao", 4096, 1420, 10, -1, 2, 23),
    ("registro_sd", 7168, 3816, 5, 0, 2, 49),
    ("wifi", 6144, 2480, 5, 0, 1, 98),
    ("IDLE0", 0, 736, 0, 0, 1, 472),
)

PREVIA = """
<aside class="preview-toolbar" aria-label="Controles da prévia local">
  <div><strong>Prévia local da equipe</strong><span>Dados e comandos simulados</span></div>
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
        self.sensores_ativos = TODOS_SENSORES
        self.nivel_global_logs = 3
        self.apenas_avisos_projeto = False
        self.cores_logs = True
        self.fontes_logs = [self.nivel_global_logs] * QUANTIDADE_FONTES_LOG
        self.tarefas_coletadas_em_ms = 0
        self.controle = dict(formato=7, atuacao_habilitada=False,
                            partida_concluida=True, armamento_inicial_pendente=False,
                            ota_em_andamento=False,
                            dac_zero_confirmado=True,
                            faixa_marcha_solicitada=0, faixa_marcha_fisica=2,
                            faixa_marcha_maxima=1,
                            reversao_solicitada=False, reversao_acionar_solicitado=False,
                            reversao_permitida=False, rearme_restante_ms=0,
                            parada_solicitada=True, corte_por_rele_solicitado=True,
                            modo_a_aterrar_solicitado=False, modo_b_aterrar_solicitado=False,
                            estado_operacao=0, bloqueios=0,
                            velocidade_maxima_centesimos_kmh=0,
                            acelerador_maximo_decimos_percentual=0,
                            acelerador_lido_decimos_percentual=0,
                            acelerador_permitido_decimos_percentual=0,
                            velocidade_medida_centesimos_kmh=0,
                            aceleracao_medida_milesimos_ms2=0,
                            parada_restante_ms=0, amostra_pedal_restante_ms=0,
                            zona_morta_decimos_percentual=50,
                            tensao_minima_saida_mv=0, tensao_pedal_mv=0,
                            tensao_dac_mv=0, dac_disponivel=False,
                            pedal_disponivel=False,
                            revisao=0, alterado_em_ms=0)
        self.parada_expira_em = None
        self.solicitacao = 0
        self.percurso_base = 0
        self.redes = ["Rede de testes", "Rede auxiliar"]
        self.principal = "Laboratório UFSM"
        self.principal_alterada = False
        self.rede = self.principal
        self.preparada = None
        self.config = dict(relatorios_pausados=False, intervalos_relatorios_ms=[1000]*4,
                           intervalo_ota_minutos=60, intervalo_telemetria_ota_ms=1000,
                           wifi_automatico=True, **POLITICA_WIFI_PADRAO)
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
                 estado_mpu=7 | (0x80 if not self.simulado else 0),
                 estado_ina=7 if not self.simulado else 0,
                 ina_shunt_microvolts=round(d["corrente_centesimos_a"] * 10),
                 ina_temperatura_centesimos_c=3250,
                 ina_energia_mwh=round(self.tempo() * 20),
                 ina_carga_mah=round(self.tempo() * 2),
                 ina_diagnostico=3,
                 temperatura_mpu_centesimos_c=3250,
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
        dados = dict(ok=True, valido=True, idade_ms=10, versao_firmware="0.0.0-previa",
                    firmware="0.0.0-previa", tempo_ativo_ms=int(self.tempo()*1000),
                    cpu_valida=True, cpu_total_decimos_percentual=cpu,
                    cpu_nucleo_0_decimos_percentual=cpu+30, cpu_nucleo_1_decimos_percentual=cpu-30,
                    temperatura_valida=True, temperatura_centesimos_c=round(4200+170*math.sin(phase)),
                    heap_livre=heap, heap_interno_livre=heap, heap_interno_total=327680,
                    heap_interno_minimo=171000, heap_interno_maior_bloco=120000,
                    psram_disponivel=True, psram_total=8388608, psram_livre=7000000,
                    tarefas_ativas=len(TAREFAS_PREVIA) if grupo == "mestre" else 14,
                    motivo_reinicio=1, motivo_ultimo_reset=1,
                    wifi_ssid="NEXUS-"+grupo, clientes_wifi=len(self.clientes.get(grupo, [])),
                    uart_recepcoes_ok=int(self.tempo()*4), taxa_pacotes_centesimos_hz=400,
                    taxa_perda_centesimos_percentual=0)
        if grupo == "mestre":
            menor_pilha = min(TAREFAS_PREVIA, key=lambda tarefa: tarefa[2])
            degradado = self.cenario == "degradado"
            dados.update(pilha_valida=True, menor_pilha_livre_bytes=menor_pilha[2],
                         tarefa_menor_pilha=menor_pilha[0], psram_maior_bloco=6200000,
                         erros_sensores=2 if degradado else 0,
                         erros_i2c=ERROS_I2C_PREVIA if degradado else 0,
                         erros_uart=0, descartes_uart=0, falhas_ota=0, falhas_supervisao=0)
        return dados

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
        if nome == "versao":
            return dict(firmware="0.0.0-previa", capacidades=CAPACIDADES_MESTRE)
        if nome in ("satelite_equipe_versao", "satelite_visitantes_versao"):
            return dict(firmware="0.0.0-previa", capacidades=16383)
        if nome == "controle_remoto":
            agora = time.monotonic()
            if self.parada_expira_em and agora >= self.parada_expira_em:
                self.controle.update(parada_solicitada=False,
                                     faixa_marcha_solicitada=min(self.controle["faixa_marcha_fisica"],self.controle["faixa_marcha_maxima"]),
                                     reversao_solicitada=False, parada_restante_ms=0)
                self.parada_expira_em = None
            data = self.telemetria()
            self.controle["velocidade_medida_centesimos_kmh"] = data["velocidade_centesimos_kmh"]
            self.controle["aceleracao_medida_milesimos_ms2"] = data["aceleracao_milesimos_ms2"]
            if self.parada_expira_em:
                self.controle["parada_restante_ms"] = max(0, math.ceil((self.parada_expira_em - agora) * 1000))
            c = self.controle
            c["rearme_restante_ms"] = max(0, math.ceil(self._espera_rearme(agora) * 1000))
            c["corte_por_rele_solicitado"] = bool(c["parada_solicitada"] or not c["faixa_marcha_solicitada"] or
                not c["velocidade_maxima_centesimos_kmh"] or not c["acelerador_maximo_decimos_percentual"])
            c["modo_a_aterrar_solicitado"] = bool(not c["corte_por_rele_solicitado"] and c["faixa_marcha_solicitada"] == 1)
            c["modo_b_aterrar_solicitado"] = bool(not c["corte_por_rele_solicitado"] and c["faixa_marcha_solicitada"] == 3)
            c["reversao_acionar_solicitado"] = bool(c["reversao_permitida"] and
                not c["corte_por_rele_solicitado"] and c["reversao_solicitada"])
            return dict(self.controle)
        if nome == "telemetria":
            return self.telemetria()
        if nome == "uart":
            return dict(recepcoes_ok=int(self.tempo()*4), falhas_crc=0, pacotes_perdidos=0,
                        equipe_online=True, visitantes_online=True)
        if nome in ("diagnostico_mestre", "satelite_visitantes_diagnostico"):
            return self.diagnostico("mestre" if nome == "diagnostico_mestre" else "visitantes")
        if nome == "saude":
            diagnostico = self.diagnostico("mestre")
            return dict(estado=2 if self.cenario == "degradado" else 0,
                        causas_ativas=8 if self.cenario == "degradado" else 0,
                        heap_livre=diagnostico["heap_livre"], tempo_ativo_ms=diagnostico["tempo_ativo_ms"],
                        erros_historicos=diagnostico["erros_sensores"] + diagnostico["erros_i2c"])
        if nome == "wifi":
            return dict(conectado=bool(self.rede), radio_ativo=True, rede=self.rede, rssi_dbm=-52, canal=6)
        if nome == "configuracao":
            return dict(self.config, quantidade_redes_wifi=len(self.redes),
                        rede_principal=self.principal,
                        principal_alterada=self.principal_alterada)
        if nome == "simulador":
            return dict(ativo=self.simulado, padrao_firmware=True, amostras_geradas=int(self.tempo()*4))
        if nome == "sensores":
            return self.sensores()
        if nome == "tarefas_mestre":
            return self.tarefas_mestre(parametros.get("inicio", ["0"])[0])
        if nome == "logs_terminal":
            limite = 2 if self.apenas_avisos_projeto else 3
            return dict(nivel_global=self.nivel_global_logs,
                        apenas_avisos_projeto=self.apenas_avisos_projeto, cores=self.cores_logs,
                        fontes=[dict(id=indice, nivel=nivel, nivel_efetivo=min(nivel, limite))
                                for indice, nivel in enumerate(self.fontes_logs)])
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

    def tarefas_mestre(self, parametro_inicio):
        if not parametro_inicio.isascii() or not parametro_inicio.isdecimal():
            raise ValueError("Início da página de tarefas inválido.")
        inicio = int(parametro_inicio)
        if inicio >= LIMITE_TAREFAS or inicio % TAREFAS_POR_PAGINA:
            raise ValueError("Início da página de tarefas fora da faixa.")
        if inicio == 0 or not self.tarefas_coletadas_em_ms:
            self.tarefas_coletadas_em_ms = max(1, round(self.tempo() * 1000))
        tarefas = []
        for nome, pilha, livre, prioridade, nucleo, estado, cpu in TAREFAS_PREVIA[inicio:inicio + TAREFAS_POR_PAGINA]:
            tarefas.append(dict(nome=nome, pilha_reservada_bytes=pilha, pilha_minima_livre_bytes=livre,
                                prioridade=prioridade, nucleo=nucleo, estado=estado,
                                cpu_decimos=cpu, cpu_valida=True))
        return dict(valido=True, coletado_em_ms=self.tarefas_coletadas_em_ms,
                    inicio=inicio, total=len(TAREFAS_PREVIA), quantidade=len(tarefas), tarefas=tarefas)

    def sensores(self):
        validos = self.sensores_ativos & (0b10000 if self.simulado else TODOS_SENSORES)
        degradado = self.cenario == "degradado"
        if degradado:
            validos &= ~0b01010  # Falha simulada nos dois sensores I²C.
        adc, mpu, encoders, ina, esp = (validos & (1 << indice) for indice in range(5))
        telemetria = self.telemetria()
        contagem = round(self.tempo() * 12) if encoders else 0
        velocidade = telemetria["velocidade_centesimos_kmh"] / 100 if encoders else 0
        return dict(
            versao_formato=1, ativos=self.sensores_ativos, validos=validos, controle_permitido=True,
            adc_bruto=1270 if adc else 0,
            acelerometro_bruto=[220, -37, 16384] if mpu else [0, 0, 0],
            giroscopio_bruto=[15, -12, 4] if mpu else [0, 0, 0],
            temperatura_mpu_c=telemetria["temperatura_mpu_centesimos_c"] / 100 if mpu else 0,
            temperatura_esp_c=self.diagnostico("mestre")["temperatura_centesimos_c"] / 100 if esp else 0,
            esquerda=dict(contagem=contagem, velocidade_kmh=velocidade),
            direita=dict(contagem=contagem, velocidade_kmh=velocidade),
            ina228=dict(
                tensao_v=telemetria["tensao_pacote_mv"] / 1000 if ina else 0,
                corrente_a=telemetria["corrente_centesimos_a"] / 100 if ina else 0,
                queda_shunt_mv=telemetria["ina_shunt_microvolts"] / 1000 if ina else 0,
                temperatura_c=telemetria["ina_temperatura_centesimos_c"] / 100 if ina else 0,
                diagnostico=telemetria["ina_diagnostico"] if ina else 0,
            ),
            i2c=dict(operacoes_ok=int(self.tempo() * 8), erros=ERROS_I2C_PREVIA if degradado else 0,
                     sondagens_ok=0 if degradado else 2, enderecos_ausentes=2 if degradado else 0,
                     ultimo_erro=263 if degradado else 0),
        )

    def ajustar_logs(self, linhas):
        if not all(parte.isascii() and parte.isdecimal() for parte in linhas[:3]):
            raise ValueError("Ajuste, valor ou fonte de log inválidos.")
        tipo, valor, alvo = map(int, linhas[:3])
        if tipo not in range(4) or valor not in range(2 if tipo in (1, 2) else 4):
            raise ValueError("Ajuste ou valor de log fora da faixa.")
        if alvo not in range(QUANTIDADE_FONTES_LOG if tipo == 3 else 1):
            raise ValueError("Fonte de log fora da faixa.")
        if tipo == 0:
            self.nivel_global_logs = valor
            self.apenas_avisos_projeto = False
            self.fontes_logs = [valor] * QUANTIDADE_FONTES_LOG
        elif tipo == 1:
            self.apenas_avisos_projeto = bool(valor)
        elif tipo == 2:
            self.cores_logs = bool(valor)
        else:
            self.fontes_logs[alvo] = valor

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

    def _espera_rearme(self, agora):
        if self.parada_expira_em is None:
            return 0
        return self.parada_expira_em - PARADA_AUTOMATICA_S + PARADA_MINIMA_S - agora

    def _ativar_parada(self, agora):
        self.parada_expira_em = agora + PARADA_AUTOMATICA_S
        self.controle.update(parada_solicitada=True, faixa_marcha_solicitada=0,
                             reversao_solicitada=False, reversao_acionar_solicitado=False,
                             corte_por_rele_solicitado=True, acelerador_permitido_decimos_percentual=0,
                             parada_restante_ms=PARADA_AUTOMATICA_S * 1000,
                             rearme_restante_ms=PARADA_MINIMA_S * 1000)

    def comando(self, nome, corpo):
        if self.cenario == "sem-mestre":
            raise ConnectionError("Cenário de prévia: mestre sem resposta.")
        lines = corpo.split("\n")
        if nome in ("definir_marcha", "aceleracao_maxima", "definir_acelerador"):
            raise ValueError("Comando antigo não disponível.")
        fields = {"limite_marcha": ("faixa_marcha_maxima", 3),
                  "definir_reversao": ("reversao_solicitada", 1),
                  "permitir_re": ("reversao_permitida", 1),
                  "velocidade_maxima": ("velocidade_maxima_centesimos_kmh", 3500),
                  "limite_acelerador": ("acelerador_maximo_decimos_percentual", 1000),
                  "zona_morta_pedal": ("zona_morta_decimos_percentual", 300),
                  "tensao_minima_dac": ("tensao_minima_saida_mv", 2500),
                  "parada_remota": ("parada_solicitada", 1)}
        if nome == "definir_faixa_marcha":
            raise ValueError("A marcha é escolhida na chave física.")
        if nome in fields:
            field, maximum = fields[nome]
            if not corpo.isascii() or not corpo.isdecimal() or int(corpo) > maximum:
                raise ValueError("Valor fora da faixa permitida.")
            value = int(corpo)
            c = self.controle
            if nome == "limite_marcha" and value == 0:
                raise ValueError("A faixa máxima deve ser L, M ou H.")
            if nome == "definir_reversao":
                if value and not c["reversao_permitida"]:
                    raise ValueError("Ré bloqueada.")
                if c["parada_solicitada"]:
                    raise ValueError("Rearme antes de trocar o sentido.")
            if nome in ("zona_morta_pedal", "tensao_minima_dac") and c["acelerador_lido_decimos_percentual"]:
                raise ValueError("Solte o pedal antes de alterar a calibração.")
            if nome == "parada_remota":
                agora = time.monotonic()
                if value == 0 and self._espera_rearme(agora) > 0:
                    raise ValueError("Aguarde 5 s após a parada para rearmar.")
            c[field] = bool(value) if nome in ("parada_remota", "definir_reversao", "permitir_re") else value
            if nome == "parada_remota":
                if value:
                    self._ativar_parada(agora)
                else:
                    self.parada_expira_em = None
                    c.update(faixa_marcha_solicitada=min(c["faixa_marcha_fisica"], c["faixa_marcha_maxima"]),
                             reversao_solicitada=False, acelerador_permitido_decimos_percentual=0,
                             parada_restante_ms=0, rearme_restante_ms=0)
            if nome == "permitir_re" and not value and (c["reversao_solicitada"] or c["reversao_acionar_solicitado"]):
                self._ativar_parada(time.monotonic())
            if nome == "limite_marcha" and not c["parada_solicitada"]:
                c["faixa_marcha_solicitada"] = min(c["faixa_marcha_fisica"],value)
            c["revisao"] += 1
            c["alterado_em_ms"] = int(self.tempo()*1000)
            return {}
        protected = {"wifi_alterar_principal": 1, "wifi_principal_salva": 2,
                     "wifi_restaurar_principal": 1, "wifi_restaurar_politica": 1,
                     "wifi_automatico": 2, "wifi_politica": 5, "logs_terminal": 4}
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
        elif nome == "sensores":
            if len(lines) != 2 or lines[0] not in ("0", "1", "2", "3", "4", "255") or lines[1] not in ("0", "1"):
                raise ValueError("Sensor ou modo inválido.")
            mascara = TODOS_SENSORES if lines[0] == "255" else 1 << int(lines[0])
            if lines[1] == "1":
                self.sensores_ativos |= mascara
            else:
                self.sensores_ativos &= ~mascara
        elif nome == "logs_terminal":
            self.ajustar_logs(lines)
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
            if len(lines) != 2 or not 1 <= len(lines[0].encode()) <= 32 or not (len(lines[1].encode()) == 0 or 8 <= len(lines[1].encode()) <= 63):
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
            elif nome == "wifi_alterar_principal":
                self.principal = self.preparada
                self.principal_alterada = True
                self.redes = [ssid for ssid in self.redes if ssid != self.principal]
                self.rede = self.principal
                self.preparada = None
            else:
                self.rede = self.preparada
        elif nome == "wifi_principal_salva":
            try:
                index = int(lines[0])
            except ValueError as exc:
                raise ValueError("Índice da rede salva inválido.") from exc
            if not 1 <= index <= len(self.redes):
                raise ValueError("Rede salva inexistente.")
            self.principal = self.redes.pop(index-1)
            self.principal_alterada = True
            self.rede = self.principal
        elif nome in ("wifi_conectar_salva","wifi_excluir_salva"):
            index = int(corpo)
            if not 1 <= index <= len(self.redes):
                raise ValueError("Rede salva inexistente.")
            if nome == "wifi_conectar_salva":
                self.rede = self.redes[index-1]
            else:
                self.redes.pop(index-1)
        elif nome == "wifi_restaurar_principal":
            self.principal = "Laboratório UFSM"
            self.principal_alterada = False
            self.rede = self.principal
        elif nome == "wifi_automatico":
            if not lines or lines[0] not in ("0", "1"):
                raise ValueError("Modo de conexão inválido.")
            self.config["wifi_automatico"] = lines[0] == "1"
        elif nome == "wifi_politica":
            values = [int(v) for v in lines[:4]]
            if len(values) != 4 or not (1 <= values[0] <= 10 and 2000 <= values[1] <= 30000 and 100 <= values[2] <= 5000 and 5000 <= values[3] <= 180000):
                raise ValueError("Política fora da faixa da prévia.")
            self.config.update(zip(POLITICA_WIFI_PADRAO, values))
        elif nome == "wifi_restaurar_politica":
            self.config.update(POLITICA_WIFI_PADRAO)
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
    adicionar_opcoes_acesso(parser, 8766)
    parser.add_argument("--cenario", choices=("normal","degradado","sem-mestre"), default="normal")
    args = parser.parse_args()
    if not 1 <= args.porta <= 65535:
        parser.error("A porta deve estar entre 1 e 65535.")
    endereco = selecionar_endereco(args.rede)
    with criar_servidor(endereco, args.porta, SimuladorEquipe(args.cenario)) as servidor:
        servir_previa(servidor)


if __name__ == "__main__":
    main()
