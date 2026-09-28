// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The LineageOS Project
#pragma once
#include "BcmFmProtocol.h"
#include <atomic>
#include <mutex>

namespace bcmfm {
using Bytes = std::vector<uint8_t>;
class Bus {
public:
    virtual ~Bus() = default;
    virtual bool open() = 0;
    virtual void close() = 0;
    virtual bool exchange(const Bytes& request, Bytes* response) = 0;
};
class Clock {
public:
    virtual ~Clock() = default;
    virtual int64_t nowMs() = 0;
    virtual void sleepMs(int ms) = 0;
};

// All frequencies are integral kHz on a 100-kHz EU grid. Register values use
// kHz minus 64000. Keep floating-point/JNI conversion outside this engine.
class Radio {
public:
    Radio(Bus& bus, Clock& clock) : bus_(bus), clock_(clock) {}
    bool powerUp(int khz);
    bool powerDown();
    bool tune(int khz);
    int seek(int fromKHz, bool up);
    std::vector<int> scan();
    void cancel() { ++cancel_; }  // lock-free: must interrupt a blocked scan/seek
    bool mute(bool muted);
    bool volume(int value);      // 0..255, receiver digital gain
    bool alive();
private:
    enum class Search { Found, BandLimit, Error, Cancelled, Timeout };
    bool write(uint8_t reg, int value, int width = 1);
    bool read(uint8_t reg, int width, int* value);
    bool down();
    Search search(int khz, bool seeking, bool up, uint64_t generation, int* found);
    bool tuneLocked(int khz, uint64_t generation);
    int seekLocked(int khz, bool up, bool wrap, uint64_t generation);
    void restore(int khz);
    Bus& bus_;
    Clock& clock_;
    std::mutex mutex_;
    std::atomic<uint64_t> cancel_{0};
    bool powered_ = false;
    bool connected_ = false;
    bool muted_ = true;
    int frequency_ = kLowKHz;
};
}  // namespace bcmfm
