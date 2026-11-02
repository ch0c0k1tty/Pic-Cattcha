#pragma once
#include <stdint.h>

// Pink "disposable camera" look applied in place to a w*h RGB565 buffer
// (big-endian byte order, as delivered by esp32-camera): contrast, pink tint,
// grain (seeded, so deterministic) and vignette. Pure C, no hardware needed.
void film_filter_apply(uint8_t *buf, int w, int h, uint32_t seed);
