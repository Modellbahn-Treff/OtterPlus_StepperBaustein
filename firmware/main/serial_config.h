// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Modellbahn-Treff for Kids GmbH

#pragma once

// Start the serial configuration console task.
// Call once from app_main() after nvs_flash_init().
void serial_config_start(void);
