// Relógio monotônico usado pelos serviços do Mestre.
#pragma once

#include <cstdint>

#include "esp_timer.h"

namespace tempo_monotonico {

inline int64_t agora_ms() {
    return esp_timer_get_time() / 1000;
}

inline uint32_t agora_ms32() {
    return static_cast<uint32_t>(agora_ms());
}

}  // namespace tempo_monotonico
