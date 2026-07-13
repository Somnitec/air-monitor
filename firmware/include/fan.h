#pragma once
#include <stdint.h>

// PWM setup on PIN_FAN_PWM. Call once from setup().
void fanInit();

// Direct override, 0-100. Also used internally by FAN_MANUAL_TEST_MODE.
void fanSetDutyPct(float pct);

// Re-evaluate fan duty from the latest CO2 reading. Call whenever a fresh
// SEN66 reading lands; co2==0 (no reading yet) is a no-op. No-op if
// FAN_RAMP_TEST_MODE is active.
void fanControlStep(uint16_t co2);

// Drives FAN_RAMP_TEST_MODE. Call every loop() iteration (not gated on the
// slow/CO2 channel) so the step timing is a clean wall-clock cadence.
void fanTick();
