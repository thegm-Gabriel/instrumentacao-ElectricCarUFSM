"""Contratos de versionamento do preparador OTA, sem compilar ou publicar."""

import unittest

from ferramentas.preparar_release_ota import (
    calcular_novas_versoes,
    incrementar_versao,
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


if __name__ == "__main__":
    unittest.main()
