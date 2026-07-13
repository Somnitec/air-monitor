#include "fan.h"
#include "config.h"
#include <Arduino.h>

static const uint8_t FAN_PWM_CHANNEL = 0;   // arbitrary; nothing else on this core uses LEDC
static float s_dutyPct = 0.0f;

void fanInit() {
    ledcSetup(FAN_PWM_CHANNEL, FAN_PWM_FREQ_HZ, FAN_PWM_RESOLUTION_BITS);
    ledcAttachPin(PIN_FAN_PWM, FAN_PWM_CHANNEL);
    fanSetDutyPct(0);
}

void fanSetDutyPct(float pct) {
    pct = constrain(pct, 0.0f, 100.0f);
    s_dutyPct = pct;
    const uint32_t maxDuty = (1UL << FAN_PWM_RESOLUTION_BITS) - 1;
    ledcWrite(FAN_PWM_CHANNEL, (uint32_t)(pct / 100.0f * maxDuty + 0.5f));
}

void fanControlStep(uint16_t co2) {
#if FAN_CONTROL_ENABLED && !FAN_RAMP_TEST_MODE
    if (co2 == 0) return;   // no fresh SEN66 reading yet

#if FAN_MANUAL_TEST_MODE
    fanSetDutyPct(FAN_TEST_DUTY_PCT);
#else
    if (co2 <= FAN_CO2_FLOOR_PPM) {
        fanSetDutyPct(FAN_DUTY_FLOOR_PCT);
    } else if (co2 >= FAN_CO2_CEILING_PPM) {
        fanSetDutyPct(FAN_DUTY_CEILING_PCT);
    } else {
        float frac = (float)(co2 - FAN_CO2_FLOOR_PPM)
                   / (float)(FAN_CO2_CEILING_PPM - FAN_CO2_FLOOR_PPM);
        fanSetDutyPct(FAN_DUTY_FLOOR_PCT + frac * (FAN_DUTY_CEILING_PCT - FAN_DUTY_FLOOR_PCT));
    }
#endif
#else
    (void)co2;
#endif
}

void fanTick() {
#if FAN_CONTROL_ENABLED && FAN_RAMP_TEST_MODE
    static uint32_t s_lastStepMs = 0;
    static int s_pct = 0;
    static int s_dir = 1;

    uint32_t now = millis();
    if (s_lastStepMs != 0 && now - s_lastStepMs < FAN_RAMP_STEP_MS) return;
    s_lastStepMs = now;

    fanSetDutyPct((float)s_pct);
    Serial.printf("[fan] ramp duty=%d%%\n", s_pct);

    s_pct += s_dir * FAN_RAMP_STEP_PCT;
    if (s_pct >= 100) { s_pct = 100; s_dir = -1; }
    if (s_pct <= 0)   { s_pct = 0;   s_dir = 1; }
#endif
}
