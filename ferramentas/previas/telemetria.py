"""Telemetria simulada compartilhada pelas prévias de Equipe e Visitantes."""

import time


def telemetria_simulada(inicio: float) -> dict:
    segundos = max(0.0, time.monotonic() - inicio)
    semente = int(inicio * 1_000_000) & 0xffffffff
    episodio, fase_s = divmod(min(segundos, 220.0), 45.0)
    episodio = int(episodio)

    def sorteio(indice: int, canal: int) -> float:
        valor = (semente ^ (indice * 0x9e3779b9) ^ (canal * 0x85ebca6b)) & 0xffffffff
        valor ^= valor >> 16
        valor = (valor * 0x7feb352d) & 0xffffffff
        valor ^= valor >> 15
        valor = (valor * 0x846ca68b) & 0xffffffff
        valor ^= valor >> 16
        return (valor & 0xffffff) / 16777216

    def plano(indice: int):
        re = indice != 0 and sorteio(indice, 7) < 0.14
        pausa = 1 + 2 * sorteio(indice, 2)
        pausa_final = 1 + 3 * sorteio(indice, 4)
        alvos = [0.0, (3 + 5 * sorteio(indice, 1)) if re else
                 (9 + 19 * sorteio(indice, 1))]
        for trecho in range(2, 6):
            amplitude = 7 if re else (16 if trecho == 2 else 23)
            alvos.append(max(2 if re else 5, min(9 if re else 38,
                         alvos[-1] + (sorteio(indice, 10 + trecho) - 0.5) * amplitude)))
        if indice == 0 and not re:
            alvos[5] = max(alvos[5], 20)
        alvos.append(0.0)
        pesos = [(0.45 + 0.2 * sorteio(indice, 20 + trecho))
                 if trecho == 5 and indice == 0 else
                 (0.6 + sorteio(indice, 20 + trecho)) for trecho in range(6)]
        duracao_ativa = 45 - pausa - pausa_final
        duracoes = [duracao_ativa * peso / sum(pesos) for peso in pesos]
        return re, pausa, duracoes, alvos

    def trecho_metros(inicio_kmh, fim_kmh, duracao_s, fracao):
        # Integral exata da interpolação cúbica da velocidade.
        return duracao_s * (inicio_kmh * fracao + (fim_kmh - inicio_kmh) *
                            (fracao ** 3 - 0.5 * fracao ** 4)) / 3.6

    percurso = 1200.0
    for indice in range(episodio):
        _, _, duracoes_ant, alvos_ant = plano(indice)
        percurso += sum(trecho_metros(alvos_ant[i], alvos_ant[i + 1], duracoes_ant[i], 1)
                        for i in range(6))

    re, pausa, duracoes, alvos = plano(episodio)
    marcha_ativa = 3 if re else 2
    marcha = 0
    velocidade = aceleracao = acelerador = freio = 0.0
    fase_s -= pausa
    if fase_s >= 0:
        for i, duracao in enumerate(duracoes):
            if fase_s >= duracao:
                percurso += trecho_metros(alvos[i], alvos[i + 1], duracao, 1)
                fase_s -= duracao
                continue
            fracao = max(0, fase_s / duracao)
            variacao = alvos[i + 1] - alvos[i]
            velocidade = alvos[i] + variacao * fracao ** 2 * (3 - 2 * fracao)
            aceleracao = variacao * 6 * fracao * (1 - fracao) / duracao / 3.6
            percurso += trecho_metros(alvos[i], alvos[i + 1], duracao, fracao)
            marcha = marcha_ativa
            if aceleracao > 0.03:
                acelerador = min(100, 11 + velocidade * 0.25 + aceleracao * 28)
            elif aceleracao < -0.16:
                freio = min(100, 8 - aceleracao * 30)
            elif aceleracao < -0.03:
                pass  # Desaceleração leve por inércia.
            elif velocidade > 1:
                acelerador = 8 + velocidade * 0.42
            break

    carga = max(0.0, 88.0 - int(segundos / 0.25) * 0.1)
    if carga == 0.0:
        marcha = 0
        velocidade = aceleracao = acelerador = freio = 0.0
    corrente = 0.0 if carga == 0.0 else (
        -freio * 0.12 * min(1, velocidade / 10) if freio else
        (2 + acelerador * 0.28 if acelerador else 0.8))
    tensao = 13.1 - (100 - carga) * 0.006 - corrente * 0.004
    potencia = round(tensao * corrente)
    return {
        "valido": True,
        "idade_ms": 0,
        "sequencia": int(segundos * 2) + 1,
        "simulado": True,
        "eletrica_disponivel": True,
        "marcha": marcha,
        "velocidade_centesimos_kmh": round(velocidade * 100),
        "acelerador_decimos_percentual": round(acelerador * 10),
        "freio_decimos_percentual": round(freio * 10),
        "aceleracao_disponivel": True,
        "aceleracao_milesimos_ms2": round(aceleracao * 1000),
        "tensao_pacote_mv": round(tensao * 1000),
        "corrente_centesimos_a": round(corrente * 100),
        "carga_decimos_percentual": round(carga * 10),
        "autonomia_decimos_km": round(carga * 0.62 * 10),
        "potencia_w": potencia,
        "percurso_metros": round(percurso),
    }
