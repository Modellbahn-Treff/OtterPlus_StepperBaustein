// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Modellbahn-Treff for Kids GmbH

#pragma once

// Initialise GPIO inputs based on current pin_cfg[] settings.
// Call once after settings_load_from_nvs() and stepper_init().
void gpio_inputs_init(void);

// Re-apply pin_cfg[] after a settings change (uninstalls old ISRs, reconfigures).
void gpio_inputs_reinit(void);
