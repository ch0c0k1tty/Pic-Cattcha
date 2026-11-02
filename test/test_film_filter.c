// Host-side tests for the pink film filter. No hardware needed: `make -C test`.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../main/film_filter.h"
#include "../main/focus.h"

static int failures, checks;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { failures++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

#define W 64
#define H 48

static void fill(uint8_t *buf, uint16_t px)
{
    for (int i = 0; i < W * H; i++) { buf[2 * i] = px >> 8; buf[2 * i + 1] = px & 0xFF; }
}
static uint16_t get(const uint8_t *buf, int x, int y)
{
    const uint8_t *p = buf + 2 * (y * W + x);
    return (p[0] << 8) | p[1];
}
// average of a block, 8-bit scaled channels
static void avg(const uint8_t *buf, int x0, int y0, int n, double *r, double *g, double *b)
{
    double sr = 0, sg = 0, sb = 0;
    for (int y = y0; y < y0 + n; y++)
        for (int x = x0; x < x0 + n; x++) {
            uint16_t px = get(buf, x, y);
            sr += ((px >> 11) & 0x1F) << 3; sg += ((px >> 5) & 0x3F) << 2; sb += (px & 0x1F) << 3;
        }
    *r = sr / (n * n); *g = sg / (n * n); *b = sb / (n * n);
}

static void test_pink_tint_on_gray(void)
{
    static uint8_t buf[W * H * 2];
    fill(buf, 0x8410);                       // mid gray
    film_filter_apply(buf, W, H, 1);
    double r, g, b;
    avg(buf, W / 2 - 4, H / 2 - 4, 8, &r, &g, &b);   // centre, vignette ~none
    CHECK(r > g + 20, "red should exceed green (pink cast)");
    CHECK(b > g, "blue should exceed green (pink, not orange)");
    CHECK(r + 20 >= b, "red must stay close to blue (pink/magenta, not blue)");
}

static void test_vignette_darkens_corners(void)
{
    static uint8_t buf[W * H * 2];
    fill(buf, 0x8410);
    film_filter_apply(buf, W, H, 7);
    double cr, cg, cb, kr, kg, kb;
    avg(buf, W / 2 - 4, H / 2 - 4, 8, &cr, &cg, &cb);
    avg(buf, 0, 0, 8, &kr, &kg, &kb);
    CHECK(kr + kg + kb < cr + cg + cb, "corner should be darker than centre");
}

static void test_deterministic_and_seeded(void)
{
    static uint8_t a[W * H * 2], b[W * H * 2], c[W * H * 2];
    fill(a, 0x6B4D); fill(b, 0x6B4D); fill(c, 0x6B4D);
    film_filter_apply(a, W, H, 42);
    film_filter_apply(b, W, H, 42);
    film_filter_apply(c, W, H, 43);
    CHECK(memcmp(a, b, sizeof a) == 0, "same seed gives identical output");
    CHECK(memcmp(a, c, sizeof a) != 0, "different seed gives different grain");
}

static void test_extremes_do_not_wrap(void)
{
    static uint8_t buf[W * H * 2];
    fill(buf, 0xFFFF);                       // white
    film_filter_apply(buf, W, H, 3);
    double r, g, b;
    avg(buf, W / 2 - 4, H / 2 - 4, 8, &r, &g, &b);
    CHECK(r > 200, "white stays bright, no overflow wrap");
    fill(buf, 0x0000);                       // black
    film_filter_apply(buf, W, H, 3);
    avg(buf, W / 2 - 4, H / 2 - 4, 8, &r, &g, &b);
    CHECK(r < 90 && r >= 0, "black stays dark (faded, not blown out)");
    CHECK(r > g, "black gets pink lift in red over green");
}

static void test_no_out_of_bounds(void)
{
    // guard bytes around the buffer must be untouched, odd sizes must not crash
    for (int w = 1; w <= 7; w++) {
        for (int h = 1; h <= 7; h++) {
            size_t n = (size_t)w * h * 2;
            uint8_t *raw = malloc(n + 16);
            memset(raw, 0xA5, n + 16);
            film_filter_apply(raw + 8, w, h, 9);
            int ok = 1;
            for (int i = 0; i < 8; i++) ok &= raw[i] == 0xA5 && raw[8 + n + i] == 0xA5;
            CHECK(ok, "filter wrote outside buffer");
            free(raw);
        }
    }
}

static void test_zero_seed(void)
{
    static uint8_t buf[W * H * 2];
    fill(buf, 0x8410);
    film_filter_apply(buf, W, H, 0x1234ABCD);   // XORs rng to 0; must not lock up grain
    int distinct = 0;
    uint16_t first = get(buf, W / 2, H / 2);
    for (int x = W / 2; x < W / 2 + 16; x++) distinct += get(buf, x, H / 2) != first;
    CHECK(distinct > 0, "grain still varies when seed cancels the constant");
}

static void test_focus_score(void)
{
    static uint8_t flat[W * H * 2], sharp[W * H * 2], soft[W * H * 2];
    fill(flat, 0x8410);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint16_t hard = (x / 2) % 2 ? 0xFFFF : 0x0000;      // 2px stripes: hard edges
            uint16_t mid  = (x / 8) % 2 ? 0xA514 : 0x7BCF;      // gentle blurry bands
            uint8_t *a = sharp + 2 * (y * W + x), *b = soft + 2 * (y * W + x);
            a[0] = hard >> 8; a[1] = hard & 0xFF; b[0] = mid >> 8; b[1] = mid & 0xFF;
        }
    uint32_t f = focus_score(flat, W, H), s = focus_score(sharp, W, H), b = focus_score(soft, W, H);
    CHECK(f == 0, "flat image scores 0");
    CHECK(s > b, "sharp edges score higher than blurry bands");
    CHECK(b > f, "blurry bands score above flat");
}

int main(void)
{
    test_focus_score();
    test_pink_tint_on_gray();
    test_vignette_darkens_corners();
    test_deterministic_and_seeded();
    test_extremes_do_not_wrap();
    test_no_out_of_bounds();
    test_zero_seed();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
