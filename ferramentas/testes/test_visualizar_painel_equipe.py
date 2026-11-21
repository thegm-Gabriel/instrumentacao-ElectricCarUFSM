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
from visualizar_painel_equipe import GERADOR, POLITICA_WIFI_PADRAO, SimuladorEquipe, criar_servidor


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

    def test_rede_aberta_pode_ser_preparada_e_salva(self):
        self.sim.comando("wifi_preparar", "Rede aberta\n")
        self.sim.comando("wifi_conectar_preparada", "")
        self.sim.comando("wifi_salvar_preparada", "")
        self.assertIn("Rede aberta", self.sim.redes)

    def test_rede_salva_pode_virar_principal(self):
        rede = self.sim.redes[0]
        self.sim.comando("wifi_principal_salva", "1\npermissao-ficticia")
        config = self.sim.consulta("configuracao", {})
        self.assertEqual(config["rede_principal"], rede)
        self.assertTrue(config["principal_alterada"])
        self.assertNotIn(rede, self.sim.redes)
        self.sim.comando("wifi_restaurar_principal", "permissao-ficticia")
        self.assertEqual(self.sim.consulta("configuracao", {})["rede_principal"],
                         "Laboratório UFSM")

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

    def test_carga_simulada_cai_um_decimo_a_cada_250_ms(self):
        self.assertEqual(self.sim.telemetria()["carga_decimos_percentual"], 880)
        self.advance(.249)
        self.assertEqual(self.sim.telemetria()["carga_decimos_percentual"], 880)
        self.advance(.001)
        self.assertEqual(self.sim.telemetria()["carga_decimos_percentual"], 879)
        self.advance(219.75)
        ultima = self.sim.telemetria()
        self.assertEqual(ultima["carga_decimos_percentual"], 0)
        self.assertEqual(ultima["velocidade_centesimos_kmh"], 0)
        self.assertEqual(ultima["corrente_centesimos_a"], 0)

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
            self.sim.comando("definir_faixa_marcha", "1")
        self.sim.comando("velocidade_maxima", "2500")
        self.sim.comando("limite_acelerador", "500")
        self.sim.comando("parada_remota", "0")
        self.assertEqual(self.sim.controle["faixa_marcha_solicitada"], 1)
        self.sim.comando("limite_marcha", "2")
        self.assertEqual(self.sim.controle["faixa_marcha_solicitada"], 2)
        self.sim.comando("parada_remota", "1")
        self.assertEqual(self.sim.controle["faixa_marcha_solicitada"], 0)
        self.advance(5)
        self.sim.comando("parada_remota", "0")
        self.assertEqual(self.sim.controle["faixa_marcha_solicitada"], 2)
        self.sim.comando("velocidade_maxima", "0")
        self.assertEqual(self.sim.controle["faixa_marcha_solicitada"], 2)
        fresh = SimuladorEquipe().controle
        self.assertTrue(fresh["parada_solicitada"])
        self.assertEqual(fresh["velocidade_maxima_centesimos_kmh"], 0)

    def test_rearme_exige_cinco_segundos_desde_a_ultima_parada(self):
        self.sim.comando("parada_remota", "1")
        self.advance(4)
        self.sim.comando("parada_remota", "1")
        self.advance(4.999)
        antes = dict(self.sim.controle)
        with self.assertRaisesRegex(ValueError, "5 s"):
            self.sim.comando("parada_remota", "0")
        self.assertEqual(self.sim.controle, antes)
        self.now.return_value = 1009
        self.sim.comando("parada_remota", "0")
        self.assertFalse(self.sim.controle["parada_solicitada"])

    def test_controle_remoto_recusa_valores_sem_mutacao(self):
        original = dict(self.sim.controle)
        for name, body in (("velocidade_maxima", "3501"), ("limite_acelerador", "1001"),
                           ("limite_marcha", "0"), ("parada_remota", "2"),
                           ("velocidade_maxima", "-1"), ("velocidade_maxima", "2.5"),
                           ("definir_faixa_marcha", "1\n")):
            with self.subTest(name=name, body=body), self.assertRaises(ValueError):
                self.sim.comando(name, body)
        self.assertEqual(self.sim.controle, original)

    def test_reversao_e_limite_do_pedal_na_previa(self):
        self.sim.comando("velocidade_maxima", "2000")
        self.sim.comando("limite_acelerador", "400")
        self.assertEqual(self.sim.controle["acelerador_maximo_decimos_percentual"], 400)
        self.sim.comando("parada_remota", "0")
        with self.assertRaisesRegex(ValueError, "Ré bloqueada"):
            self.sim.comando("definir_reversao", "1")
        self.sim.comando("permitir_re", "1")
        self.assertTrue(self.sim.controle["reversao_permitida"])
        self.assertFalse(self.sim.controle["reversao_solicitada"])
        self.sim.comando("definir_reversao", "1")
        self.assertEqual(self.sim.controle["faixa_marcha_solicitada"], 1)
        self.sim.comando("permitir_re", "0")
        self.assertFalse(self.sim.controle["reversao_permitida"])
        self.assertTrue(self.sim.controle["parada_solicitada"])
        self.assertEqual(self.sim.consulta("controle_remoto", {})["rearme_restante_ms"], 5000)
        self.assertEqual(self.sim.controle["acelerador_permitido_decimos_percentual"], 0)
        self.assertEqual(self.sim.controle["faixa_marcha_solicitada"], 0)
        self.assertFalse(self.sim.controle["reversao_solicitada"])

    def test_configuracao_pedal_na_previa(self):
        self.sim.comando('zona_morta_pedal', '75')
        self.sim.comando('tensao_minima_dac', '850')
        estado = self.sim.consulta('controle_remoto', {})
        self.assertEqual(estado['formato'], 7)
        self.assertEqual(estado['zona_morta_decimos_percentual'], 75)
        self.assertEqual(estado['tensao_minima_saida_mv'], 850)
        self.assertFalse(estado['atuacao_habilitada'])
        with self.assertRaises(ValueError):
            self.sim.comando('zona_morta_pedal', '301')
        with self.assertRaises(ValueError):
            self.sim.comando('tensao_minima_dac', '2501')


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

    def test_ina_aparece_na_telemetria_real_da_previa(self):
        status, html = self.request("/")
        self.assertEqual(status, 200)
        self.assertIn('id="ina-energy"', html)
        self.sim.simulado = False
        status, dados = self.request("/api/telemetria")
        self.assertEqual(status, 200)
        self.assertTrue(dados["estado_mpu"] & 0x80)
        self.assertEqual(dados["estado_ina"], 7)
        self.assertIn("ina_temperatura_centesimos_c", dados)
        self.assertIn("ina_energia_mwh", dados)

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
        self.assertEqual(self.request("/api/comandos/acesso")[0], 404)
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
        self.assertEqual(self.request('/api/mestre?consulta=definir_faixa_marcha', '1')[0], 400)
        self.request('/__previa/cenario', 'sem-mestre')
        self.assertEqual(self.request('/api/mestre?consulta=parada_remota', '1')[0], 503)

    def test_sensores_http_distingue_aquisicao_e_leitura_valida(self):
        rota = "/api/mestre?consulta=sensores"
        status, sensores = self.request(rota)
        self.assertEqual(status, 200)
        self.assertTrue(sensores["controle_permitido"])
        self.assertEqual(sensores["ativos"], 31)
        self.assertEqual(sensores["validos"], 16)
        self.request("/api/mestre?consulta=simulador_telemetria", "0")
        self.assertEqual(self.request(rota, "3\n0")[0], 200)
        sensores = self.request(rota)[1]
        self.assertEqual(sensores["ativos"], 23)
        self.assertEqual(sensores["validos"], 23)
        self.assertEqual(sensores["ina228"]["tensao_v"], 0)
        self.assertEqual(self.request(rota, "5\n1")[0], 400)
        self.assertEqual(self.request(rota, "255\n0")[0], 200)
        self.assertEqual(self.request(rota)[1]["ativos"], 0)
        self.assertEqual(self.request(rota, "255\n1")[0], 200)
        self.assertEqual(self.request(rota)[1]["validos"], 31)

    def test_logs_terminal_http_perfil_e_fonte(self):
        rota = "/api/mestre?consulta=logs_terminal"
        self.assertEqual(self.request(rota, "1\n1\n0\n")[0], 400)
        self.assertEqual(self.request(rota, "1\n1\n1\nteste")[0], 400)
        self.assertEqual(self.request(rota, "3\n1\n18\nteste")[0], 400)
        self.assertEqual(self.request(rota, "3\n1\n17\nteste")[0], 200)
        self.assertEqual(self.request(rota, "1\n1\n0\nteste")[0], 200)
        self.assertEqual(self.request(rota, "2\n0\n0\nteste")[0], 200)
        logs = self.request(rota)[1]
        self.assertEqual(len(logs["fontes"]), 18)
        self.assertEqual(logs["fontes"][0], dict(id=0, nivel=3, nivel_efetivo=2))
        self.assertEqual(logs["fontes"][17], dict(id=17, nivel=1, nivel_efetivo=1))
        self.assertTrue(logs["apenas_avisos_projeto"])
        self.assertFalse(logs["cores"])
        self.assertEqual(self.request(rota, "0\n2\n0\nteste")[0], 200)
        logs = self.request(rota)[1]
        self.assertFalse(logs["apenas_avisos_projeto"])
        self.assertEqual(logs["nivel_global"], 2)
        self.assertEqual({fonte["nivel"] for fonte in logs["fontes"]}, {2})

    def test_restauracao_http_afeta_apenas_politica_wifi(self):
        rota_mestre = "/api/mestre?consulta="
        versao = self.request(rota_mestre+"versao")[1]
        novos_comandos = 16384 | 32768 | 65536 | 131072
        self.assertEqual(versao["capacidades"] & novos_comandos, novos_comandos)
        self.request(rota_mestre+"wifi_principal_salva", "1\nteste")
        self.request(rota_mestre+"wifi_automatico", "0\nteste")
        self.request(rota_mestre+"wifi_politica", "4\n20000\n1000\n100000\nteste")
        rota = rota_mestre+"wifi_restaurar_politica"
        self.assertEqual(self.request(rota, "")[0], 400)
        alterada = self.request(rota_mestre+"configuracao")[1]
        self.assertEqual(alterada["tentativas_wifi"], 4)
        self.assertEqual(self.request(rota, "teste")[0], 200)
        restaurada = self.request(rota_mestre+"configuracao")[1]
        for campo, padrao in POLITICA_WIFI_PADRAO.items():
            self.assertEqual(restaurada[campo], padrao)
        for campo in ("wifi_automatico", "rede_principal", "quantidade_redes_wifi"):
            self.assertEqual(restaurada[campo], alterada[campo])

    def test_tarefas_mestre_http_pagina_e_preserva_coleta(self):
        rota = "/api/mestre?consulta=tarefas_mestre"
        status, primeira = self.request(rota)
        self.assertEqual(status, 200)
        self.assertTrue(primeira["valido"])
        self.assertEqual(primeira["inicio"], 0)
        self.assertEqual(primeira["quantidade"], 3)
        segunda = self.request(rota+"&inicio=3")[1]
        self.assertEqual(segunda["coletado_em_ms"], primeira["coletado_em_ms"])
        self.assertEqual(segunda["total"], primeira["total"])
        self.assertNotEqual(segunda["tarefas"][0]["nome"], primeira["tarefas"][0]["nome"])
        self.assertEqual(self.request(rota+"&inicio=6")[1]["quantidade"], 2)
        self.assertEqual(self.request(rota+"&inicio=45")[1]["tarefas"], [])
        for inicio in ("-3", "1", "48", "x"):
            with self.subTest(inicio=inicio):
                self.assertEqual(self.request(rota+"&inicio="+inicio)[0], 400)


if __name__ == "__main__":
    unittest.main()
