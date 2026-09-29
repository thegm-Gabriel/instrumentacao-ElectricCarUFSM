# Testes do protocolo de telemetria

Esta suíte verifica tamanho e serialização do quadro, CRC-16, limites de
conteúdo, confirmação dos satélites, sequência com perda/duplicação/retorno e
recuperação do fluxo depois de um quadro incompleto ou corrompido.

Os testes ficam em `test/` e não são incluídos no firmware normal. Para
executá-los futuramente, use o executor de testes do PlatformIO no ambiente do
mestre. A criação desta suíte não altera o processo normal de compilação.
