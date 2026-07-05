// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Modellbahn-Treff for Kids GmbH

#pragma once
#include <stdint.h>
#include <stdbool.h>

// Initialise TMC2208 UART, configure registers, leave driver disabled.
void  stepper_init(void);

// Set motor speed in RPM. Sign determines direction.
// Enables the driver if not already running. Range: -120..+120 RPm (hardware limit ~1800).
// Passing 0.0f coasts (VACTUAL=0) but keeps the driver enabled.
void  stepper_set_rpm(float rpm);

// Stop motion (VACTUAL = 0) and disable driver (EN = HIGH, coils released).
void  stepper_stop(void);

// Return the currently commanded RPM (0 when stopped).
float stepper_get_rpm(void);

// True if the driver is enabled and a non-zero RPM is commanded.
bool  stepper_is_running(void);
