#pragma once
#include <stdint.h>

// Initialize the PDM MEMS microphone (MSM261D4030H1CPM) via I2S PDM RX.
//   clk_pin  — PDM clock output (GPIO 45 on Waveshare ESP32-S3 1.8" LCD)
//   data_pin — PDM data  input  (GPIO 46)
// Spawns an internal FreeRTOS task on Core 0 that reads audio continuously.
// Returns false if the I2S channel cannot be created (port already in use etc.)
bool mic_pdm_init(int clk_pin, int data_pin);

// Smoothed amplitude 0–255.  Written by the mic task at ~30 Hz on Core 0.
// Core 1 can read this with no mutex — single-byte volatile access is atomic.
extern volatile uint8_t g_mic_level;
