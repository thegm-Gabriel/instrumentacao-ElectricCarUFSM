"""Contratos de versionamento do preparador OTA, sem compilar ou publicar."""

import io
import sys
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
from contextlib import redirect_stderr
from unittest.mock import patch

from ferramentas.preparar_release_ota import (
    calcular_impressao_digital,
    calcular_novas_versoes,
    escolher_incremento,
    incrementar_versao,
    main,
)


class VersoesSemVerTest(unittest.TestCase):
    def setUp(self):
        self.estado = {
            "versao_release": "2.7.9",
            "firmwares": {
                "mestre-s3": {"versao": "1.5.8"},
                "equipe-s3": {"versao": "0.9.4"},
                "visitantes-s3": {"versao": "3.2.1"},
            },
        }

    def test_cada_tipo_reseta_as_partes_menores(self):
        self.assertEqual(incrementar_versao("2.7.9", "major"), "3.0.0")
        self.assertEqual(incrementar_versao("2.7.9", "minor"), "2.8.0")
        self.assertEqual(incrementar_versao("2.7.9", "patch"), "2.7.10")
        self.assertEqual(incrementar_versao("v2.7.9", "patch"), "2.7.10")

    def test_so_firmware_alterado_recebe_bump(self):
        release, firmwares = calcular_novas_versoes(
            self.estado, {"equipe-s3"}, "minor"
        )
        self.assertEqual(release, "2.8.0")
        self.assertEqual(firmwares, {
            "mestre-s3": "1.5.8",
            "equipe-s3": "0.10.0",
            "visitantes-s3": "3.2.1",
        })

    def test_sem_alteracoes_preserva_release_e_firmwares(self):
        release, firmwares = calcular_novas_versoes(self.estado, set(), "major")
        self.assertEqual(release, "2.7.9")
        self.assertEqual(firmwares["equipe-s3"], "0.9.4")

    def test_tipo_ou_versao_invalida_falha_antes_de_preparar(self):
        with self.assertRaises(ValueError):
            incrementar_versao("1.2.3", "build")
        with self.assertRaises(ValueError):
            incrementar_versao("1.2", "major")
        with self.assertRaises(ValueError):
            incrementar_versao("01.2.3", "major")
        with self.assertRaises(ValueError):
            calcular_novas_versoes(self.estado, set(), "build")

    def test_menu_aceita_tipos_e_repete_opcao_invalida(self):
        for resposta, esperado in (("1", "patch"), ("2", "minor"),
                                   ("3", "major"), ("MiNoR", "minor")):
            with self.subTest(resposta=resposta):
                saida = []
                self.assertEqual(escolher_incremento(lambda _: resposta, saida.append), esperado)
        respostas = iter(("invalida", "2"))
        saida = []
        self.assertEqual(escolher_incremento(lambda _: next(respostas), saida.append), "minor")
        self.assertTrue(any("Opção inválida" in linha for linha in saida))

    def test_menu_cancela_sem_preparar(self):
        self.assertIsNone(escolher_incremento(lambda _: "0", lambda _: None))
        def fim_da_entrada(_):
            raise EOFError
        self.assertIsNone(escolher_incremento(fim_da_entrada, lambda _: None))
        def interrupcao(_):
            raise KeyboardInterrupt
        self.assertIsNone(escolher_incremento(interrupcao, lambda _: None))

    def test_modo_nao_interativo_exige_bump_e_opcao_explicita_dispensa_menu(self):
        with patch.object(sys, "argv", ["preparar_release_ota.py"]), \
             patch.object(sys, "stdin") as entrada, \
             patch("ferramentas.preparar_release_ota.preparar_release") as preparar, \
             redirect_stderr(io.StringIO()) as erros:
            entrada.isatty.return_value = False
            self.assertEqual(main(), 1)
            self.assertIn("--bump", erros.getvalue())
            preparar.assert_not_called()
        with patch.object(sys, "argv", ["preparar_release_ota.py", "--bump", "minor"]), \
             patch.object(sys, "stdin") as entrada, \
             patch("ferramentas.preparar_release_ota.preparar_release") as preparar:
            entrada.isatty.return_value = False
            self.assertEqual(main(), 0)
            preparar.assert_called_once()
            self.assertEqual(preparar.call_args.args[2], "minor")

    def test_cancelamento_no_main_nao_inicia_preparacao(self):
        with patch.object(sys, "argv", ["preparar_release_ota.py"]), \
             patch.object(sys, "stdin") as entrada, \
             patch("ferramentas.preparar_release_ota.escolher_incremento", return_value=None), \
             patch("ferramentas.preparar_release_ota.preparar_release") as preparar:
            entrada.isatty.return_value = True
            self.assertEqual(main(), 0)
            preparar.assert_not_called()

    def test_fontes_web_e_logo_alteram_somente_hash_dos_satelites(self):
        with TemporaryDirectory(prefix=".tmp-release-", dir=Path(__file__).resolve().parents[2]) as pasta:
            raiz = Path(pasta)
            equipe = raiz / "firmware" / "equipe-s3"
            mestre = raiz / "firmware" / "mestre-s3"
            logo = raiz / "firmware" / "compartilhado" / "web" / "logo_carro_eletrico.png"
            (equipe / "web").mkdir(parents=True)
            mestre.mkdir(parents=True)
            logo.parent.mkdir(parents=True)
            pagina = equipe / "web" / "app.js"
            pagina.write_text("const valor = 1;", encoding="utf-8")
            logo.write_bytes(b"logo inicial")
            antes_equipe = calcular_impressao_digital(raiz, equipe)
            antes_mestre = calcular_impressao_digital(raiz, mestre)
            pagina.write_text("const valor = 2;", encoding="utf-8")
            self.assertNotEqual(antes_equipe, calcular_impressao_digital(raiz, equipe))
            self.assertEqual(antes_mestre, calcular_impressao_digital(raiz, mestre))
            depois_pagina = calcular_impressao_digital(raiz, equipe)
            logo.write_bytes(b"logo atualizado")
            self.assertNotEqual(depois_pagina, calcular_impressao_digital(raiz, equipe))
            self.assertEqual(antes_mestre, calcular_impressao_digital(raiz, mestre))


if __name__ == "__main__":
    unittest.main()
