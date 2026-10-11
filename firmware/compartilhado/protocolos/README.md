# Protocolos compartilhados

Esta pasta contém estruturas binárias e regras que precisam ser idênticas no mestre,
no módulo da equipe e no módulo de visitantes.

`protocolo_telemetria.h` define o quadro v4 de 105 bytes, com versão, tipo,
tamanho da carga e CRC-16/CCITT-FALSE. Os três firmwares devem usar a mesma
revisão deste arquivo.
Além dos valores brutos, a versão 4 transporta aceleração em g, giroscópio em
graus por segundo, aceleração longitudinal filtrada, temperatura e o estado de
calibração/eixo do MPU6050.
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

As operações do menu serial do mestre usam um único comando operacional
estruturado. O campo `acao` identifica a operação e os campos tipados carregam
índices, intervalos ou credenciais; isso evita criar um código de protocolo para
cada botão da interface. Consultas de configuração e de redes salvas permanecem
separadas por não terem efeito colateral. A repetição de uma solicitação devolve
a resposta memorizada e não executa a ação novamente.
