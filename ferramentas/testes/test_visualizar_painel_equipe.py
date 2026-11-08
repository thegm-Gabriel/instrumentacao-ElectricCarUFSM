"""Contrato HTTP e transições de estado da prévia, sem ESP ou serviços externos."""

import json
import sys
import threading
import unittest
from pathlib import Path
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import Request, build_opener, ProxyHandler

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from visualizar_painel_equipe import SimuladorEquipe, criar_servidor, GERADOR, AUTORIZACAO_PREVIA


class SimuladorTests(unittest.TestCase):
    def setUp(self):
        self.clock = patch("visualizar_painel_equipe.time.monotonic", return_value=1000)
        self.now = self.clock.start()
        self.addCleanup(self.clock.stop)
        self.sim = SimuladorEquipe()

    def advance(self, seconds):
        self.now.return_value += seconds

    def test_sync_progresso_conclusao_e_nova_solicitacao(self):
        self.assertEqual(self.sim.comando("sincronizar_google", "")["sincronizacao_id"], 1)
        with self.assertRaises(ValueError):
            self.sim.comando("sincronizar_google", "")
        self.advance(5)
        data = self.sim.sincronizacao()
        self.assertTrue(data["em_andamento"])
        self.assertEqual(data["arquivos_upload_concluidos"], 1)
        self.assertEqual(data["arquivo_atual_bytes_enviados"], 16384)
        self.advance(7)
        data = self.sim.sincronizacao()
        self.assertFalse(data["em_andamento"])
        self.assertEqual(data["sincronizacoes_finalizadas"], 1)
        self.assertEqual(data["arquivos_pendentes"], 0)
        self.assertEqual(self.sim.comando("sincronizar_google", "")["sincronizacao_id"], 2)

    def test_ota_exige_oferta_e_expira_sem_autorizacao(self):
        with self.assertRaises(ValueError):
            self.sim.comando("autorizar_todos", "")
        self.sim.comando("verificar_ota", "")
        self.assertEqual(self.sim.ota()["mestre"]["estado"], 3)
        self.advance(2)
        self.assertEqual(self.sim.ota()["equipe"]["estado"], 4)
        self.advance(60)
        self.assertEqual(self.sim.ota()["estado"], 8)
        with self.assertRaises(ValueError):
            self.sim.comando("autorizar_todos", "")

    def test_ota_alvos_individuais_progresso_e_versao_persistente(self):
        self.sim.comando("verificar_ota", "")
        self.advance(2)
        self.sim.comando("autorizar_equipe", "")
        self.advance(5)
        self.assertEqual(self.sim.ota()["equipe"]["progresso_percentual"], 50)
        self.advance(5)
        self.assertEqual(self.sim.ota()["equipe"]["estado"], 7)
        self.sim.comando("autorizar_todos", "")
        self.assertNotIn("equipe", self.sim.ota_alvos)
        self.advance(10)
        self.assertEqual(self.sim.ota()["mestre"]["versao_atual"], "0.0.1-previa")
        self.sim.comando("verificar_ota", "")
        self.advance(2)
        self.assertEqual(self.sim.ota()["estado"], 4)

    def test_wifi_preparar_conectar_salvar_excluir(self):
        with self.assertRaises(ValueError):
            self.sim.comando("wifi_conectar_preparada", "")
        self.sim.comando("wifi_preparar", "Rede fictícia\nsenha-ficticia")
        self.sim.comando("wifi_conectar_preparada", "")
        self.sim.comando("wifi_salvar_preparada", "")
        self.assertEqual(self.sim.consulta("wifi", {})["rede"], "Rede fictícia")
        self.assertEqual(self.sim.consulta("rede_wifi_salva", {"indice": ["3"]})["ssid"], "Rede fictícia")
        self.sim.comando("wifi_excluir_salva", "3")
        self.assertEqual(len(self.sim.redes), 2)
        self.assertNotIn("senha-ficticia", repr(vars(self.sim)))

    def test_configuracao_protegida_validada_antes_de_alterar(self):
        original = dict(self.sim.config)
        for body in ("3\n15000\n3000\n120000", "11\n15000\n3000\n120000\nteste"):
            with self.assertRaises(ValueError):
                self.sim.comando("wifi_politica", body)
        self.assertEqual(self.sim.config, original)
        self.sim.comando("wifi_politica", "4\n20000\n1000\n100000\nteste")
        self.assertEqual(self.sim.config["tentativas_wifi"], 4)
        self.sim.comando("wifi_automatico", "0\nteste")
        self.assertFalse(self.sim.config["wifi_automatico"])
        self.sim.comando("intervalo_relatorio", "2\n500")
        self.assertEqual(self.sim.config["intervalos_relatorios_ms"][2], 500)

    def test_clientes_kick_e_expiracao_afetam_diagnostico(self):
        mac = self.sim.wifi_clientes("equipe")["clientes"][0]["mac"]
        self.sim.wifi_acao("equipe", "kick", mac)
        self.assertEqual(self.sim.diagnostico("equipe")["clientes_wifi"], 1)
        self.sim.wifi_acao("visitantes", "limite", 0)
        self.advance(900)
        self.assertEqual(self.sim.diagnostico("equipe")["clientes_wifi"], 0)
        self.assertEqual(self.sim.wifi_clientes("visitantes")["quantidade"], 2)

    def test_fonte_percurso_e_telemetria_4hz(self):
        first = self.sim.telemetria()["sequencia"]
        self.advance(.25)
        self.assertEqual(self.sim.telemetria()["sequencia"], first+1)
        self.sim.comando("zerar_percurso", "")
        self.assertEqual(self.sim.telemetria()["percurso_metros"], 0)
        self.sim.comando("simulador_telemetria", "0")
        self.assertFalse(self.sim.telemetria()["simulado"])

    def test_trajetoria_variada_e_sinais_coerentes(self):
        leituras = []
        for _ in range(180):
            leituras.append(self.sim.telemetria())
            self.advance(.25)
        velocidades = [d["velocidade_centesimos_kmh"] / 100 for d in leituras]
        variacoes = [(b - a) / .25 for a, b in zip(velocidades, velocidades[1:])]
        self.assertTrue(any(taxa > 3 for taxa in variacoes))
        self.assertTrue(any(taxa < -3.5 for taxa in variacoes))
        self.assertTrue(any(0 < taxa < 1 for taxa in variacoes))
        self.assertTrue(any(-1 < taxa < 0 for taxa in variacoes))
        self.assertTrue(any(
            max(velocidades[max(0, i-12):i]) - velocidades[i] >= 1.5
            and variacoes[i-1] <= -3.5
            for i in range(1, len(velocidades))
        ))
        self.assertTrue(any(d["freio_decimos_percentual"] > 0 for d in leituras))
        self.assertTrue(any(d["acelerador_decimos_percentual"] > 0 for d in leituras))
        self.assertTrue(all(0 <= v <= 38 for v in velocidades))
        self.assertEqual(sorted(d["percurso_metros"] for d in leituras),
                         [d["percurso_metros"] for d in leituras])
        for d in leituras:
            if d["freio_decimos_percentual"] > 0:
                self.assertEqual(d["acelerador_decimos_percentual"], 0)
                self.assertLess(d["aceleracao_milesimos_ms2"], 0)
                self.assertLessEqual(d["corrente_centesimos_a"], 0)
            corrente = d["corrente_centesimos_a"] / 100
            tensao = d["tensao_pacote_mv"] / 1000
            self.assertAlmostEqual(d["potencia_w"], corrente * tensao, delta=1)

    def test_controle_remoto_intertravamentos_e_parada_sem_retomada(self):
        self.assertFalse(self.sim.consulta("controle_remoto", {})["atuacao_habilitada"])
        with self.assertRaises(ValueError):
            self.sim.comando("definir_marcha", "1")
        self.sim.comando("velocidade_maxima", "2500")
        self.sim.comando("aceleracao_maxima", "1200")
        self.sim.comando("parada_remota", "0")
        self.sim.comando("definir_marcha", "1")
        with self.assertRaises(ValueError):
            self.sim.comando("definir_marcha", "2")
        self.sim.comando("parada_remota", "1")
        self.sim.comando("parada_remota", "0")
        self.assertEqual(self.sim.controle["marcha_solicitada"], 0)
        self.sim.comando("definir_marcha", "2")
        self.sim.comando("velocidade_maxima", "0")
        self.assertEqual(self.sim.controle["marcha_solicitada"], 0)
        fresh = SimuladorEquipe().controle
        self.assertTrue(fresh["parada_solicitada"])
        self.assertEqual(fresh["velocidade_maxima_centesimos_kmh"], 0)

    def test_controle_remoto_recusa_valores_sem_mutacao(self):
        original = dict(self.sim.controle)
        for name, body in (("velocidade_maxima", "3501"), ("aceleracao_maxima", "5001"),
                           ("definir_marcha", "3"), ("parada_remota", "2"),
                           ("velocidade_maxima", "-1"), ("velocidade_maxima", "2.5"),
                           ("definir_marcha", "1\n")):
            with self.subTest(name=name, body=body), self.assertRaises(ValueError):
                self.sim.comando(name, body)
        self.assertEqual(self.sim.controle, original)


class HttpTests(unittest.TestCase):
    def setUp(self):
        self.sim = SimuladorEquipe()
        self.server = criar_servidor("127.0.0.1", 0, self.sim)
        self.worker = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.worker.start()
        self.addCleanup(self.close)
        self.base = f"http://127.0.0.1:{self.server.server_port}"
        self.client = build_opener(ProxyHandler({}))

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.worker.join(timeout=2)

    def request(self, path, body=None, authorized=True):
        request = Request(self.base+path, data=body.encode() if body is not None else None)
        if authorized and (path == "/api/comandos/acesso" or
                           (body is not None and (path.startswith("/api/mestre?") or
                                                  path.startswith("/api/wifi/")))):
            request.add_header("Authorization", AUTORIZACAO_PREVIA)
        try:
            response = self.client.open(request, timeout=5)
        except HTTPError as error:
            response = error
        with response:
            data = response.read().decode("utf-8")
            return response.status, json.loads(data) if "json" in response.headers["Content-Type"] else data

    def test_html_incorpora_fontes_e_faixa_apenas_na_previa(self):
        status, html = self.request("/")
        self.assertEqual(status, 200)
        self.assertIn("preview-scenario", html)
        self.assertIn("device-glance", html)
        self.assertNotIn("/* NEXUS_", html)
        self.assertNotIn("preview-scenario", GERADOR.montar_html())
        self.assertEqual(self.request("/painel")[0], 200)

    def test_consultas_do_painel_e_erros_de_entrada(self):
        for query in ("ping", "versao", "telemetria", "uart", "ota", "saude", "wifi",
                      "sincronizacao", "simulador", "configuracao", "diagnostico_mestre",
                      "satelite_equipe_ping", "satelite_equipe_versao",
                      "satelite_visitantes_ping", "satelite_visitantes_versao",
                      "satelite_visitantes_diagnostico", "satelite_visitantes_clientes"):
            with self.subTest(query=query):
                status, data = self.request("/api/mestre?consulta="+query)
                self.assertEqual(status, 200)
                self.assertTrue(data["ok"])
        self.assertEqual(self.request("/api/mestre?consulta=inexistente")[0], 400)
        self.assertEqual(self.request("/api/wifi/limite", "{malformado")[0], 400)
        self.assertEqual(self.request("/api/wifi/limite", '{"minutos":1441}')[0], 400)
        self.assertEqual(self.request("/nao-existe")[0], 404)

    def test_comandos_http_e_cenarios(self):
        self.assertEqual(self.request("/api/comandos/acesso", authorized=False)[0], 401)
        self.assertEqual(self.request("/api/comandos/acesso")[0], 200)
        self.assertEqual(self.request("/api/mestre?consulta=sincronizar_google", "", authorized=False)[0], 401)
        status, data = self.request("/api/mestre?consulta=sincronizar_google", "")
        self.assertEqual(status, 200)
        self.assertEqual(data["sincronizacao_id"], 1)
        self.assertEqual(self.request("/api/mestre?consulta=sincronizacao")[1]["em_andamento"], True)
        self.assertEqual(self.request("/api/wifi/limite", '{"minutos":20}')[0], 200)
        self.assertEqual(self.request("/api/wifi/clientes")[1]["limite_sessao_minutos"], 20)
        self.request("/__previa/cenario", "degradado")
        self.assertEqual(self.request("/api/mestre?consulta=saude")[1]["estado"], 2)
        self.request("/__previa/cenario", "sem-mestre")
        self.assertEqual(self.request("/api/mestre?consulta=ping")[0], 503)
        self.assertEqual(self.request("/api/mestre?consulta=verificar_ota", "")[0], 503)
        self.assertEqual(self.request("/api/diagnostico")[0], 200)
        self.assertFalse(self.request("/api/telemetria")[1]["valido"])
        self.request("/__previa/cenario", "normal")
        self.assertEqual(self.request("/api/mestre?consulta=ping")[0], 200)

    def test_controle_remoto_http_confirmacao_e_erro(self):
        self.assertEqual(self.request('/api/mestre?consulta=velocidade_maxima', '2300')[0], 200)
        status, data = self.request('/api/mestre?consulta=controle_remoto')
        self.assertEqual(status, 200)
        self.assertEqual(data['velocidade_maxima_centesimos_kmh'], 2300)
        self.assertFalse(data['atuacao_habilitada'])
        self.assertEqual(self.request('/api/mestre?consulta=definir_marcha', '1')[0], 400)
        self.request('/__previa/cenario', 'sem-mestre')
        self.assertEqual(self.request('/api/mestre?consulta=parada_remota', '1')[0], 503)


if __name__ == "__main__":
    unittest.main()
