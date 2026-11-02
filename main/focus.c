#include "focus.h"

static inline int green(const uint8_t *buf, int w, int x, int y)
{
    const uint8_t *p = buf + 2 * ((size_t)y * w + x);
    uint16_t px = ((uint16_t)p[0] << 8) | p[1];
    return (px >> 5) & 0x3F;
}

uint32_t focus_score(const uint8_t *buf, int w, int h)
{
    int x0 = w / 4, x1 = w - w / 4, y0 = h / 4, y1 = h - h / 4;
    if (x1 - x0 < 2 || y1 - y0 < 2) return 0;
    uint64_t sum = 0;
    for (int y = y0; y < y1 - 1; y++) {
        for (int x = x0; x < x1 - 1; x++) {
            int g = green(buf, w, x, y);
            int dx = g - green(buf, w, x + 1, y);
            int dy = g - green(buf, w, x, y + 1);
            sum += (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
        }
    }
    uint64_t count = (uint64_t)(x1 - x0 - 1) * (y1 - y0 - 1);
    return (uint32_t)(sum * 100 / count);
}
