/*
 * model.h - gas classifier
 * Copyright (c) 2026 Nimal Danyath K (nimaldanyathk). All rights reserved.
 *
 * Placeholder rules for now, picked from how the sensors react.
 * Run ml/train.py on recorded data and it overwrites this file
 * with the trained tree. Nothing else needs to change.
 *
 * classes: 0 clean, 1 LPG, 2 smoke, 3 steam, 4 alcohol/VOC
 */
#ifndef MODEL_H
#define MODEL_H

#include <stdint.h>

#define MODEL_IS_TRAINED  0

static uint8_t model_classify(const float *f)
{
    /* all three sensors near their clean-air value */
    if (f[0] > 0.85f && f[1] > 0.85f && f[2] > 0.85f) return 0;   /* CLEAN */

    /* MQ-6 fell much more than MQ-135: LPG signature */
    if (f[3] < 0.80f) return 1;                                   /* LPG   */

    /* humidity jumped and the sensors fell together: steam */
    if (f[9] > 8.0f && f[3] > 0.80f && f[3] < 1.25f) return 3;   /* STEAM */

    /* MQ-2 fell more than MQ-6: smoke signature */
    if (f[4] < 0.85f) return 2;                                   /* SMOKE */

    /* MQ-135 fell the most: alcohol / VOC signature */
    if (f[3] > 1.15f) return 4;                                   /* VOC   */

    /* all three fell together, roughly in proportion: steam */
    return 3;                                                     /* STEAM */
}

#endif /* MODEL_H */
