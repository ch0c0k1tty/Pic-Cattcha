#pragma once
#include <stdint.h>
#include <stdbool.h>

#define PHOTO_DIR "/photos"   // FAT partition in on-board flash

// Start the BLE "photo lab": advertises as "Pic-Cattcha" and serves stored photos.
// on_new_roll: called when the phone asks for a fresh roll; returns the new shots-left count.
void ble_cattcha_start(uint8_t (*on_new_roll)(void));
// Tell connected phone that the roll / photo list changed.
void ble_cattcha_set_shots_left(uint8_t shots_left);
// True while a phone is connected.
bool ble_cattcha_is_connected(void);
