# Protocolos compartilhados

Esta pasta contém estruturas binárias e regras que precisam ser idênticas no mestre,
no módulo da equipe e no módulo de visitantes.

`protocolo_telemetria.h` define o quadro v3 de 88 bytes, com versão, tipo,
tamanho da carga e CRC-16/CCITT-FALSE. Os três firmwares devem usar a mesma
revisão deste arquivo.
Qualquer alteração no formato deve ser aplicada primeiro neste arquivo.

Cada satélite responde a um quadro válido com uma confirmação de 20 bytes. Ela
repete a sequência e o tempo do mestre para permitir a medição do tempo de ida
e volta sem sincronização de relógios, usando o mesmo CRC-16. A resposta também
informa tipo do satélite, versão do firmware e capacidades disponíveis.

`protocolo_comandos.h` define o transporte bidirecional de comandos. O quadro é
genérico: contém código, identificador da solicitação, resultado e uma carga
binária variável protegida por CRC-32. Assim, novos comandos e respostas podem
ser incluídos sem criar outro enquadramento UART. Respostas evolutivas devem
indicar sua versão na própria carga ou continuar reconhecíveis pelo tamanho.

A resposta detalhada de estado OTA usa o formato 2 e informa separadamente o
mestre, o satélite da equipe e o satélite de visitantes. A estrutura antiga foi
mantida para permitir a atualização gradual dos dispositivos.
