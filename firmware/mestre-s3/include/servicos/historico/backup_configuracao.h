// Backup de configuração em cartão já montado.
#pragma once

#include <cstdint>

namespace backup_configuracao {

// O registro serializa a recuperação na montagem com as atualizações periódicas.
void recuperar();
void atualizar(uint32_t instante_ms);

}  // namespace backup_configuracao
