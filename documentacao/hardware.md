# Projetos de hardware e revisões

[Índice da documentação](README.md) · [Pinagem do firmware](pinagem.md)

Os arquivos KiCad estão em `hardware/kicad/KiCad_pcb_telemetria/`. Há vários
projetos e versões de placa nessa pasta. **O repositório não identifica qual
delas foi fabricada ou instalada no carro.** O nome de um arquivo não comprova
que o esquema, a PCB e a montagem física correspondem à mesma revisão.

| Grupo de arquivos | Identificação disponível no repositório |
|---|---|
| `KiCad_pcb_telemetria.kicad_pro/.kicad_sch/.kicad_pcb` | Projeto com esquema e PCB de mesmo nome. |
| `NEXUSv01.kicad_pro/.kicad_pcb` | Outra versão de projeto e PCB. |
| `PLACAV1`, `PLACAV2-TESTE`, `PLACAV3` (`.kicad_pro/.kicad_pcb`) | Revisões nomeadas de PCB; “TESTE” faz parte do nome, não é uma homologação registrada aqui. |
| `PLACA_MODELO_pcb.kicad_pro/.kicad_pcb` | Projeto de placa identificado como modelo. |
| `01_alimentacao`, `comunicacaoWiFi`, `controleDoDriver`, `esp32s3Mestre`, `periféricos` (`.kicad_sch`) | Folhas de esquema presentes na pasta. |
| `TELEMETRIA-COPIA.kicad_sch` | Cópia de esquema sem indicação de revisão principal. |

Os arquivos em `KiCad_pcb_telemetria-backups/` são cópias automáticas de
trabalho. Arquivos `.kicad_prl` guardam preferências locais do editor, e
`.lck` é um bloqueio de sessão; nenhum desses arquivos define a placa
validada. Este guia não modifica nem descarta os projetos ou backups.

Antes de montar, alterar GPIOs ou fabricar uma PCB, registre qual conjunto de
arquivos corresponde à placa física, compare conectores e trilhas com a
[pinagem](pinagem.md) e confira níveis de alimentação em bancada. Ao confirmar
uma revisão, anote aqui o nome do projeto, a identificação impressa na placa e
o estado da validação elétrica. Até isso acontecer, trate todas as revisões
como referências a verificar.
