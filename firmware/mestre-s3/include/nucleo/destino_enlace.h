// Identifica os destinos disponíveis no enlace com os satélites.
#pragma once

#include <cstdint>

// Identidade lógica dos módulos conectados. Não pertence ao driver UART:
// serviços e protocolos podem referenciar os destinos sem depender do hardware.
enum class DestinoUart : uint8_t {
    Equipe,
    Visitantes,
    Todos,
};
