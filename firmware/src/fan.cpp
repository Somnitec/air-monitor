#include "fan.h"
#include "config.h"
#include <Arduino.h>
#include "driver/ledc.h"

static const uint8_t FAN_PWM_CHANNEL = 0;   // arbitrary; nothing else on this core uses LEDC
// Arduino core 2.x puts channels 0-7 in LEDC group 0 = high-speed on classic ESP32.
static const ledc_mode_t FAN_LEDC_MODE = LEDC_HIGH_SPEED_MODE;
static float s_targetDutyPct = 0.0f;  // where the current fade (if any) ends

static uint32_t dutyCounts(float pct) {
    const uint32_t maxDuty = (1UL << FAN_PWM_RESOLUTION_BITS) - 1;
    return (uint32_t)(pct / 100.0f * maxDuty + 0.5f);
}

void fanInit() {
    ledcSetup(FAN_PWM_CHANNEL, FAN_PWM_FREQ_HZ, FAN_PWM_RESOLUTION_BITS);
    ledcAttachPin(PIN_FAN_PWM, FAN_PWM_CHANNEL);
    ledc_fade_func_install(0);   // enables the hardware-fade calls in fanGlideTo()
    fanSetDutyPct(0);
}

void fanSetDutyPct(float pct) {
    pct = constrain(pct, 0.0f, 100.0f);
    s_targetDutyPct = pct;
    ledcWrite(FAN_PWM_CHANNEL, dutyCounts(pct));
}

// Glide to pct over FAN_DUTY_GLIDE_MS via LEDC hardware fade. If a fade is
// still in flight, ledc_set_fade_with_time blocks until it finishes — fine
// here because targets change at most once per slow read (>> glide time).
static void fanGlideTo(float pct) {
    pct = constrain(pct, 0.0f, 100.0f);
    if (pct == s_targetDutyPct) return;   // already there or already gliding there
    s_targetDutyPct = pct;
    ledc_set_fade_with_time(FAN_LEDC_MODE, (ledc_channel_t)FAN_PWM_CHANNEL,
                            dutyCounts(pct), FAN_DUTY_GLIDE_MS);
    ledc_fade_start(FAN_LEDC_MODE, (ledc_channel_t)FAN_PWM_CHANNEL, LEDC_FADE_NO_WAIT);
}

void fanControlStep(uint16_t co2) {
#if FAN_CONTROL_ENABLED && !FAN_RAMP_TEST_MODE
    if (co2 == 0) return;   // no fresh SEN66 reading yet

#if FAN_MANUAL_TEST_MODE
    fanGlideTo(FAN_TEST_DUTY_PCT);
#else
    float target;
    if (co2 <= FAN_CO2_FLOOR_PPM) {
        target = FAN_DUTY_FLOOR_PCT;
    } else if (co2 >= FAN_CO2_CEILING_PPM) {
        target = FAN_DUTY_CEILING_PCT;
    } else {
        float frac = (float)(co2 - FAN_CO2_FLOOR_PPM)
                   / (float)(FAN_CO2_CEILING_PPM - FAN_CO2_FLOOR_PPM);
        target = FAN_DUTY_FLOOR_PCT + frac * (FAN_DUTY_CEILING_PCT - FAN_DUTY_FLOOR_PCT);
    }
    fanGlideTo(target);
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
