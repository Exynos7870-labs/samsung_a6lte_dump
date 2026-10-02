// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The LineageOS Project
#pragma once

// Called by the adapter lifecycle, after HCI startup and before HCI shutdown.
// No-ops unless BRCM_FM_HCI_INCLUDED is enabled in bdroid_buildcfg.h.
void btif_bcm_fm_start();
void btif_bcm_fm_stop();
