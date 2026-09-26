# Protocolos compartilhados

Esta pasta contém estruturas binárias e regras que precisam ser idênticas no mestre,
no módulo da equipe e no módulo de visitantes.

`protocolo_telemetria.h` define o quadro atual de 23 bytes e o cálculo de checksum XOR.
Qualquer alteração no formato deve ser aplicada primeiro neste arquivo.
