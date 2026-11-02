#include "film_filter.h"

static inline uint8_t clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

void film_filter_apply(uint8_t *buf, int w, int h, uint32_t seed)
{
    uint8_t *p = buf;
    const int cx = w / 2, cy = h / 2;
    const int max_d2 = cx * cx + cy * cy ? cx * cx + cy * cy : 1;
    uint32_t rng = 0x1234ABCD ^ seed;
    if (!rng) rng = 1;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++, p += 2) {
            uint16_t px = ((uint16_t)p[0] << 8) | p[1];
            int r = (px >> 11) & 0x1F, g = (px >> 5) & 0x3F, b = px & 0x1F;
            r = (r << 3) | (r >> 2);
            g = (g << 2) | (g >> 4);
            b = (b << 3) | (b >> 2);

            // soft contrast
            r = (((r - 128) * 280) >> 8) + 128;
            g = (((g - 128) * 280) >> 8) + 128;
            b = (((b - 128) * 280) >> 8) + 128;

            // pink tint, faded blacks
            // highlights too: scale green/blue down so whites go pink instead of staying white
            g = (g * 220) >> 8;      // x0.86
            b = (b * 256) >> 8;      // x1.0 (blue kept at full strength)
            r += 36; g -= 28; b += 46;

            // grain
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            int n = (int)(rng & 0x1F) - 16;
            r += n; g += n; b += n;

            // vignette (down to ~60% at the corners)
            int dx = x - cx, dy = y - cy;
            int vig = 256 - ((dx * dx + dy * dy) * 100 / max_d2);
            r = (r * vig) >> 8;
            g = (g * vig) >> 8;
            b = (b * vig) >> 8;

            r = clamp8(r); g = clamp8(g); b = clamp8(b);
            px = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            p[0] = px >> 8;
            p[1] = px & 0xFF;
        }
    }
}
