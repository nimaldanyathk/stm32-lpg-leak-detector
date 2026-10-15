/*
 * features.h - the 10 features fed to the classifier
 * Same maths as ml/train.py, keep both in sync.
 *
 * Copyright (c) 2026 Nimal Danyath K (nimaldanyathk). All rights reserved.
 *
 *  f0 r6       MQ-6 Rs/R0 (1.0 = clean air, lower = more gas)
 *  f1 r2       MQ-2 Rs/R0
 *  f2 r135     MQ-135 Rs/R0
 *  f3 r6/r135  LPG hits MQ-6 much harder than MQ-135
 *  f4 r2/r6    smoke hits MQ-2 harder than MQ-6
 *  f5 r2/r135
 *  f6 d6       change in r6 over 3 s
 *  f7 d2       change in r2 over 3 s
 *  f8 drh      change in humidity over 3 s
 *  f9 rh_rise  humidity minus its ~5 min average (steam!)
 *
 * Humidity comes in as 0.01 %RH integers so the board and the
 * python script get exactly the same numbers. No AHT20 -> f8, f9 = 0.
 */
#ifndef FEATURES_H
#define FEATURES_H

#include <stdint.h>

#define NUM_SENSORS     3
#define S_MQ6           0
#define S_MQ2           1
#define S_MQ135         2

#define NUM_FEATURES    10
#define F_R6            0
#define F_R2            1
#define F_R135          2
#define F_R6_R135       3
#define F_R2_R6         4
#define F_R2_R135       5
#define F_D6            6
#define F_D2            7
#define F_DRH           8
#define F_RH_RISE       9

#define LAG_SAMPLES     30      /* 30 samples at 10 Hz = 3 s window      */

#define SENSOR_VC       5.0f    /* MQ heater and load supply              */
#define ADC_VREF        3.3f
#define ADC_FULL        4095.0f
#define DIVIDER_GAIN    2.0f    /* 10k / 10k divider halves the voltage   */

typedef struct {
    float   rs0[NUM_SENSORS];   /* sensor resistance in clean air         */
    float   hist6[LAG_SAMPLES]; /* last 3 s of r6                         */
    float   hist2[LAG_SAMPLES]; /* last 3 s of r2                         */
    int32_t histrh[LAG_SAMPLES];/* last 3 s of humidity, 0.01 %RH units   */
    uint8_t idx;
    uint8_t count;
} feat_state_t;

/* Sensor resistance relative to its load resistor, from one ADC count.
   The divider means the sensor's AO voltage is twice the pin voltage. */
static float rs_from_raw(uint16_t raw)
{
    float v = (float)raw * (ADC_VREF / ADC_FULL) * DIVIDER_GAIN;
    if (v < 0.01f)              v = 0.01f;
    if (v > SENSOR_VC - 0.01f)  v = SENSOR_VC - 0.01f;
    return (SENSOR_VC - v) / v;
}

/* Call once after calibration with the clean-air ADC averages. */
static void feat_reset(feat_state_t *s, const uint16_t base_raw[NUM_SENSORS])
{
    uint8_t i;
    for (i = 0; i < NUM_SENSORS; i++) s->rs0[i] = rs_from_raw(base_raw[i]);
    for (i = 0; i < LAG_SAMPLES; i++) {
        s->hist6[i] = 1.0f; s->hist2[i] = 1.0f; s->histrh[i] = 0;
    }
    s->idx   = 0;
    s->count = 0;
}

/* Call once per 100 ms sample. Fills f[0..9].
   rh_c  : humidity now, in 0.01 %RH   (0 if no AHT20)
   rhb_c : slow humidity average, same units (0 if no AHT20) */
static void feat_compute(feat_state_t *s, const uint16_t raw[NUM_SENSORS],
                         int32_t rh_c, int32_t rhb_c, float f[NUM_FEATURES])
{
    float r6   = rs_from_raw(raw[S_MQ6])   / s->rs0[S_MQ6];
    float r2   = rs_from_raw(raw[S_MQ2])   / s->rs0[S_MQ2];
    float r135 = rs_from_raw(raw[S_MQ135]) / s->rs0[S_MQ135];
    float d6 = 0.0f;
    float d2 = 0.0f;
    float drh = 0.0f;

    if (s->count >= LAG_SAMPLES) {          /* window full: real delta */
        d6 = r6 - s->hist6[s->idx];
        d2 = r2 - s->hist2[s->idx];
        drh = (float)(rh_c - s->histrh[s->idx]) / 100.0f;
    }
    s->hist6[s->idx] = r6;
    s->hist2[s->idx] = r2;
    s->histrh[s->idx] = rh_c;
    s->idx = (uint8_t)((s->idx + 1U) % LAG_SAMPLES);
    if (s->count < LAG_SAMPLES) s->count++;

    f[F_R6]      = r6;
    f[F_R2]      = r2;
    f[F_R135]    = r135;
    f[F_R6_R135] = r6 / r135;
    f[F_R2_R6]   = r2 / r6;
    f[F_R2_R135] = r2 / r135;
    f[F_D6]      = d6;
    f[F_D2]      = d2;
    f[F_DRH]     = drh;
    f[F_RH_RISE] = (float)(rh_c - rhb_c) / 100.0f;
}

#endif /* FEATURES_H */
