<div align="center">
  <img src="firmware/compartilhado/web/logo_carro_eletrico.png" alt="Símbolo do Carro Elétrico UFSM-CS" width="230">
  <h1>Carro Elétrico UFSM-CS</h1>
  <p><strong>Um laboratório em movimento para aprender, experimentar e compartilhar.</strong></p>
  <p>Universidade Federal de Santa Maria · Campus Cachoeira do Sul</p>
  <p><a href="#o-projeto">Conheça o projeto</a> · <a href="#o-que-este-repositório-contém">Explore o sistema</a> · <a href="documentacao/README.md">Leia a documentação</a></p>
</div>

---

## O projeto

O Carro Elétrico UFSM-CS nasceu da ideia de transformar um veículo em uma
**bancada didática viva**. Nele, conceitos estudados separadamente em sala de
aula se encontram: circuitos, acionamentos, programação, sistemas de energia,
medição e mecânica precisam funcionar juntos. Construir e testar o carro dá aos
estudantes a oportunidade de formular hipóteses, resolver problemas reais e
entender as consequências de cada escolha de projeto.

A proposta também aproxima a universidade da comunidade. Ao apresentar o
veículo e seus dados de forma compreensível, o projeto permite compartilhar o
que está sendo desenvolvido e despertar interesse pela engenharia e pela
mobilidade elétrica. A integração com energia fotovoltaica faz parte da visão
acadêmica do carro; este repositório descreve a instrumentação implementada,
sem presumir que todo o sistema de abastecimento já esteja instalado.

## A instrumentação

Este repositório dá ao carro meios de **observar e explicar seu funcionamento**.
O firmware coleta leituras, acompanha a saúde dos módulos, registra dados para
análise e apresenta telemetria à equipe e aos visitantes. O resultado serve
tanto ao desenvolvimento técnico quanto às demonstrações do projeto.

| Ideia do projeto | Como aparece nesta etapa |
|---|---|
| **Aprender fazendo** | Sensores, encoders, comunicação e interfaces dão material para montagem, testes e investigação. |
| **Medir para compreender** | Telemetria ao vivo, histórico no cartão SD e diagnóstico ajudam a relacionar teoria e comportamento observado. |
| **Compartilhar conhecimento** | Dois painéis web apresentam os dados com níveis de acesso adequados à equipe e ao público visitante. |

## O que este repositório contém

| Componente | Responsabilidade |
|---|---|
| **ESP32-S3 Mestre** | Lê sensores e encoders, calcula telemetria, registra CSV e diagnóstico no SD, coordena comandos, Wi-Fi externo e atualizações OTA. |
| **ESP32-S3 Equipe** | Recebe dados por UART, hospeda o painel técnico e encaminha comandos ao mestre. |
| **ESP32-S3 Visitantes** | Hospeda o painel público de leitura da telemetria recebida por UART. |
| **Código compartilhado** | Define os protocolos UART, modelos de dados e serviços comuns aos firmwares. |

Os três firmwares são projetos independentes **PlatformIO/ESP-IDF** em
[`firmware/`](firmware/). A página da Equipe separa telemetria, comandos,
controle preparatório e diagnóstico dos ESPs. A de Visitantes mostra movimento,
energia disponível e o histórico da sessão sem controles do mestre. Os detalhes
de formatos, limites e recuperação estão na documentação.

## Por onde começar

- [Visão geral e mapa da documentação](documentacao/README.md) — arquitetura,
  pinagem, funcionamento e guias por dispositivo.
- [Filosofia do código e fluxos](documentacao/filosofia.md) — responsabilidades,
  percurso dos dados, comandos e tratamento de falhas.
- [Ferramentas](documentacao/ferramentas.md) — geração das páginas, QR codes,
  compilação, testes e preparação de releases.
- [Versionamento e OTA](documentacao/versionamento.md) — significado de major,
  minor e patch e processo de publicação dos três firmwares.

Antes de conectar os módulos ou gravar por cabo, confira a
[pinagem e os níveis elétricos](documentacao/pinagem.md).

## Equipe

**Orientação:** Prof. Dr. Paulo César Vargas Luz.

**Equipe discente desta etapa de instrumentação:**

- Adylson Hahn
- Érick Ramon Biscaglia
- Gabriel Muttoni
- Gustavo Reckziegel Machado
- Théos Schultz Tompsen
