// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The LineageOS Project
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bcmfm {
// Abstract AF_UNIX/SOCK_SEQPACKET socket. HCI command/event bytes, no H4 prefix.
constexpr char kSocket[] = "a6lte.bcm_fm.v2";
constexpr uint32_t kClientUid = 1000;     // platform FMRadio's android.uid.system
constexpr uint32_t kServerUid = 1002;     // Bluetooth app, NOT the vendor HAL
constexpr size_t kMaxEvent = 257;
constexpr int kLowKHz = 87500;
constexpr int kHighKHz = 108000;
constexpr int kStepKHz = 100;
inline bool validFrequency(int khz) {
    return khz >= kLowKHz && khz <= kHighKHz && khz % kStepKHz == 0;
}
inline uint16_t le16(const uint8_t* p) { return p[0] | (uint16_t(p[1]) << 8); }

// Defence in depth: no generic HCI access, pin mux, RAM, firmware operations. RDS FIFO reads are fixed at 240 bytes maximum.
// Match every request length and value before it enters Fluoride.
inline bool allowed(const uint8_t* p, size_t n) {
    if (n < 6 || n > 7 || p[0] != 0x15 || p[1] != 0xfc || p[2] != n - 3)
        return false;
    const uint8_t reg = p[3], access = p[4];
    if (access == 1) {
        if (n != 6) return false;
        switch (reg) {
            case 0x00: case 0x01: case 0x0f: case 0x4d: case 0xdf:
                return p[5] == 1;
            case 0x80: return p[5] == 240; // bounded RDS FIFO, no arbitrary memory reads
            case 0x05: case 0x0a: case 0x12:
                return p[5] == 2;
            default: return false;
        }
    }
    if (access != 0) return false;
    if (n == 6) {
        switch (reg) {
            case 0x00: return p[5] == 0 || p[5] == 1 || p[5] == 3; // FM / FM+RDS
            case 0x02: return p[5] == 2; // RDS mode and FIFO flush
            case 0x14: return p[5] == 64; // FIFO waterline in tuples
            case 0x01: return p[5] == 0x02;    // west band, auto stereo
            case 0x07: return p[5] == 105 || p[5] == (105 | 0x80);
            case 0x08: return p[5] == 10;      // stock SNR threshold
            case 0x09: return p[5] <= 2;       // cancel, preset, seek
            case 0xfc: case 0xfe: return p[5] == 0;
            case 0xfd: return p[5] == 1;       // 100 kHz
            default: return false;
        }
    }
    const uint16_t value = le16(p + 5);
    switch (reg) {
        case 0x05: return value == 0x2d || value == 0x2f; // I2S, 50us, mute bit
        case 0x0a: return validFrequency(value + 64000);
        case 0x10: return value == 0;           // poll flags; no async interrupts
        case 0xf8: return value <= 255;
        default: return false;
    }
}

// Require the register/access echo; do not guess around incompatible firmware.
inline bool response(const std::vector<uint8_t>& request,
                     const std::vector<uint8_t>& event, std::vector<uint8_t>* data) {
    if (!allowed(request.data(), request.size()) || event.size() < 8 ||
        event[0] != 0x0e || event[1] != event.size() - 2 ||
        event[3] != 0x15 || event[4] != 0xfc || event[5] != 0 ||
        event[6] != request[3] || event[7] != request[4]) return false;
    const size_t expected = request[4] == 1 ? request[5] : 0;
    if (request[3] == 0x80 && request[4] == 1) {
        // Firmware may return an end marker / short FIFO. All other registers
        // retain exact-width validation. Echo, status and HCI length stay strict.
        if (event.size() > 8 + expected || (event.size() - 8) % 3) return false;
    } else if (event.size() != 8 + expected) return false;
    data->assign(event.begin() + 8, event.end());
    return true;
}
}  // namespace bcmfm
