#pragma once
#include <stdint.h>
#include <stddef.h>

// Sharpness score of a w*h RGB565 frame (big-endian bytes): mean absolute green-channel
// gradient over the centre half of the image, x100. Higher = sharper. 0 for a flat image.
uint32_t focus_score(const uint8_t *buf, int w, int h);

// Focus mode: start a Wi-Fi access point "Pic-Cattcha" (password "pinkcamera") and an HTTP
// server at http://192.168.4.1/ showing live frames plus the sharpness score.
void focus_mode_run(void);
