"""Contrato HTTP e transições de estado da prévia, sem ESP ou serviços externos."""

import json
import threading
import unittest
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import Request, build_opener, ProxyHandler

from visualizar_painel_equipe import SimuladorEquipe, criar_servidor, GERADOR


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

    def request(self, path, body=None):
        request = Request(self.base+path, data=body.encode() if body is not None else None)
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


if __name__ == "__main__":
    unittest.main()
