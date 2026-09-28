// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The LineageOS Project
#include "BcmFmRadio.h"
#include <algorithm>
#include <cstdio>
#include <cstdarg>
#include <set>
#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace {
void logError(const char* format, ...) {
    va_list args;
    va_start(args, format);
#ifdef __ANDROID__
    __android_log_vprint(ANDROID_LOG_ERROR, "BcmFm", format, args);
#else
    std::fputs("BcmFm: ", stderr);
    std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr);
#endif
    va_end(args);
}
}

namespace bcmfm {
bool Radio::write(uint8_t reg, int value, int width) {
    Bytes request{0x15, 0xfc, static_cast<uint8_t>(2 + width), reg, 0,
                  static_cast<uint8_t>(value)};
    if (width == 2) request.push_back(static_cast<uint8_t>(value >> 8));
    Bytes event, data;
    bool ok = allowed(request.data(), request.size()) && bus_.exchange(request, &event) &&
              response(request, event, &data);
    if (!ok) logError("register write %02x failed", reg);
    return ok;
}
bool Radio::read(uint8_t reg, int width, int* value) {
    Bytes request{0x15, 0xfc, 3, reg, 1, static_cast<uint8_t>(width)}, event, data;
    if (!bus_.exchange(request, &event) || !response(request, event, &data)) {
        logError("register read %02x failed", reg);
        return false;
    }
    *value = width == 2 ? le16(data.data()) : data[0];
    return true;
}

bool Radio::powerUp(int khz) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!validFrequency(khz)) return false;
    if (powered_) {
        const int previous = frequency_;
        if (tuneLocked(khz, cancel_.load())) return true;
        restore(previous);
        return false;
    }
    const uint64_t generation = cancel_.load();
    if (!bus_.open()) return false;
    connected_ = true;
    auto initWrite = [&](uint8_t reg, int value, uint8_t width = 1) {
        return generation == cancel_.load() && write(reg, value, width);
    };
    auto initRead = [&](uint8_t reg, uint8_t width, int* value) {
        return generation == cancel_.load() && read(reg, width, value);
    };
    // Based on the documented Broadcom register protocol, not an observed A6
    // sequence. Fail closed on any non-zero status, bad echo or unexpected size.
    bool ok = initWrite(0x00, 1);
    if (ok) clock_.sleepMs(300);
    int system = 0, pcmRoute = 0;
    ok = ok && initRead(0x00, 1, &system) && (system & 1) &&
         initRead(0x4d, 1, &pcmRoute) && !(pcmRoute & 0x80) && // don't steal FM-over-SCO
         initWrite(0x10, 0, 2) && initWrite(0x01, 0x02) && initWrite(0xfd, 1) &&
         initWrite(0x08, 10) && initWrite(0xfc, 0) && initWrite(0xfe, 0) &&
         initWrite(0xf8, 0, 2) && initWrite(0x05, 0x2f, 2);
    // The existing board/vendor I2S configuration owns pin mux/clock roles.
    // No FC61 pin writes, controller reset, firmware download or SCO reroute.
    powered_ = ok;
    muted_ = true;
    if (!ok || !tuneLocked(khz, generation)) {
        logError("power-up failed: SYS=%02x PCM_ROUTE=%02x requested=%d kHz", system, pcmRoute, khz);
        down();
        return false;
    }
    return true;
}

bool Radio::down() {
    bool ok = true;
    if (connected_) {
        // Always try off, even if mute failed or initialization was only partial.
        write(0x05, 0x2f, 2);
        ok = write(0x00, 0);
        bus_.close();
    }
    rdsEnabled_ = false;
    rds_.reset();
    powered_ = connected_ = false;
    muted_ = true;
    return ok;
}
bool Radio::powerDown() {
    cancel();
    std::lock_guard<std::mutex> lock(mutex_);
    return down();
}

Radio::Search Radio::search(int khz, bool seeking, bool up, uint64_t generation, int* found) {
    if (!powered_ || !validFrequency(khz)) return Search::Error;
    if (generation != cancel_.load()) return Search::Cancelled;
    if (!resetRds()) return Search::Error;
    int stale;
    if (!write(0x09, 0) || !read(0x12, 2, &stale) ||
        (seeking && !write(0x07, 105 | (up ? 0x80 : 0))) ||
        !write(0x0a, khz - 64000, 2) || !write(0x09, seeking ? 2 : 1)) return Search::Error;
    const auto deadline = clock_.nowMs() + (seeking ? 20000 : 2000);
    while (clock_.nowMs() < deadline) {
        if (generation != cancel_.load()) {
            write(0x09, 0);
            return Search::Cancelled;
        }
        int flags;
        if (!read(0x12, 2, &flags)) return Search::Error;
        if (flags & 2) return seeking ? Search::BandLimit : Search::Error;
        if (flags & 1) {
            int value;
            if (!read(0x0a, 2, &value) || !validFrequency(value + 64000)) {
                logError("invalid frequency readback");
                return Search::Error;
            }
            *found = value + 64000;
            if (!seeking && *found != khz) {
                logError("tune readback mismatch: requested=%d actual=%d kHz", khz, *found);
                return Search::Error;
            }
            frequency_ = *found;
            if (!resetRds()) return Search::Error;
            return Search::Found;
        }
        clock_.sleepMs(40);
    }
    write(0x09, 0);
    logError("%s timeout", seeking ? "seek" : "tune");
    return Search::Timeout;
}

bool Radio::tuneLocked(int khz, uint64_t generation) {
    int found;
    return search(khz, false, true, generation, &found) == Search::Found;
}
void Radio::restore(int khz) {
    // Cancellation only terminates the search. Restore the previous channel so
    // a failed/cancelled operation doesn't leave the UI and tuner disagreeing.
    if (!tuneLocked(khz, cancel_.load())) down();
}
bool Radio::tune(int khz) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!powered_ || !validFrequency(khz)) return false;
    const int previous = frequency_;
    if (tuneLocked(khz, cancel_.load())) return true;
    restore(previous);
    return false;
}

int Radio::seekLocked(int khz, bool up, bool wrap, uint64_t generation) {
    int next = khz + (up ? kStepKHz : -kStepKHz);
    if (next > kHighKHz || next < kLowKHz) {
        if (!wrap) return -1;
        next = up ? kLowKHz : kHighKHz;
        wrap = false;
    }
    int found = -1;
    Search result = search(next, true, up, generation, &found);
    if (result == Search::Found) return found;
    if (result == Search::BandLimit && wrap && generation == cancel_.load()) {
        result = search(up ? kLowKHz : kHighKHz, true, up, generation, &found);
        if (result == Search::Found) return found;
    }
    return -1;
}
int Radio::seek(int khz, bool up) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!powered_ || !validFrequency(khz)) return -1;
    const int previous = frequency_;
    const int found = seekLocked(khz, up, true, cancel_.load());
    if (found < 0) restore(previous);
    return found;
}

std::vector<int> Radio::scan() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!powered_) return {};
    const int previous = frequency_;
    const auto generation = cancel_.load();
    std::set<int> stations;
    int cursor = kLowKHz - kStepKHz;
    const auto deadline = clock_.nowMs() + 120000;
    // Bound both iterations and elapsed time; firmware must not spin us forever
    // by repeatedly returning the same frequency or implicitly wrapping.
    for (int n = 0; n <= (kHighKHz - kLowKHz) / kStepKHz; ++n) {
        if (generation != cancel_.load() || clock_.nowMs() >= deadline) break;
        int found;
        if (search(cursor + kStepKHz, true, true, generation, &found) != Search::Found) break;
        if (found <= cursor || !stations.insert(found).second || found == kHighKHz) break;
        cursor = found;
    }
    restore(previous);
    if (generation != cancel_.load() || !powered_) return {};
    return {stations.begin(), stations.end()};
}
bool Radio::mute(bool muted) {
    // Focus-loss callbacks can arrive on the main thread during a search. Audio
    // routing is stopped immediately by the app; don't wait behind a long seek.
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || !powered_ || !write(0x05, muted ? 0x2f : 0x2d, 2)) return false;
    muted_ = muted;
    return true;
}
bool Radio::volume(int value) {
    std::lock_guard<std::mutex> lock(mutex_);
    return powered_ && value >= 0 && value <= 255 && write(0xf8, value, 2);
}
bool Radio::alive() {
    std::lock_guard<std::mutex> lock(mutex_);
    int system;
    if (powered_ && read(0x00, 1, &system) && (system & 1)) return true;
    if (connected_) down();
    return false;
}
bool Radio::resetRds() {
    rds_.reset();
    return !rdsEnabled_ || write(0x02, 2);
}
bool Radio::fifo(RdsDecoder& decoder) {
    Bytes request{0x15, 0xfc, 3, 0x80, 1, 240}, event, data;
    return bus_.exchange(request, &event) && response(request, event, &data) &&
           decoder.feed(data, frequency_, clock_.nowMs());
}
bool Radio::setRds(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    rds_.reset();
    rdsEnabled_ = false;
    if (!powered_) return !enabled;
    if (!write(0x00, enabled ? 3 : 1) ||
        (enabled && (!write(0x02, 2) || !write(0x14, 64)))) {
        logError("RDS initialization failed");
        down(); return false;
    }
    rdsEnabled_ = enabled;
    nextAfCheck_ = 0;
    return true;
}
int Radio::rssiMagnitude() {
    int raw;
    return read(0x0f, 1, &raw) ? ((0x80 - raw) & 0x7f) : -1;
}
int Radio::pollRds() {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || !powered_ || !rdsEnabled_) return 0;
    if (!fifo(rds_)) { logError("RDS FIFO read failed"); down(); return 0; }
    rds_.expire(clock_.nowMs());
    int events = rds_.takeEvents();
    if (clock_.nowMs() >= nextAfCheck_ && clock_.nowMs() >= nextAfAttempt_ &&
        rds_.pi() && rds_.alternatives().size() > 1) {
        nextAfCheck_ = clock_.nowMs() + 2000;
        if (rssiMagnitude() >= 105) events |= RdsDecoder::kAf;
    }
    return events;
}
std::string Radio::programService() {
    std::lock_guard<std::mutex> lock(mutex_); return rds_.ps();
}
std::string Radio::radioText() {
    std::lock_guard<std::mutex> lock(mutex_); return rds_.text();
}
int Radio::activeAf() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!powered_ || !rdsEnabled_ || !rds_.pi() || clock_.nowMs() < nextAfAttempt_) return -1;
    nextAfAttempt_ = clock_.nowMs() + 30000; // no repeated audible probing
    const int oldRssi = rssiMagnitude();
    if (oldRssi < 105) return -1;
    const auto saved = rds_;
    const auto alternatives = saved.alternatives();
    if (alternatives.size() < 2) return -1;
    const auto generation = cancel_.load();
    const int original = frequency_;
    const bool wasMuted = muted_;
    if (!write(0x05, 0x2f, 2)) { down(); return -1; }
    muted_ = true;
    int selected = -1;
    const auto deadline = clock_.nowMs() + 5000;
    for (size_t n = 0; n < std::min(size_t(2), alternatives.size()); ++n) {
        const int candidate = alternatives[afCursor_++ % alternatives.size()];
        if (generation != cancel_.load() || clock_.nowMs() >= deadline) break;
        if (candidate == original || !tuneLocked(candidate, generation)) continue;
        int strength = rssiMagnitude();
        if (strength < 0 || strength > oldRssi - 6) continue;
        RdsDecoder identity;
        const auto piDeadline = std::min(deadline, clock_.nowMs() + 1200);
        while (clock_.nowMs() < piDeadline && generation == cancel_.load()) {
            if (!fifo(identity)) break;
            if (identity.pi()) {
                if (identity.pi() == saved.pi()) selected = candidate;
                break;
            }
            clock_.sleepMs(80);
        }
        if (selected > 0) break;
    }
    // Never leave the app/UI tuned to an unapproved or different-PI station.
    restore(original);
    if (!powered_) return -1;
    rds_ = saved;
    muted_ = wasMuted || generation != cancel_.load();
    if (!write(0x05, muted_ ? 0x2f : 0x2d, 2)) { down(); return -1; }
    return generation == cancel_.load() ? selected : -1;
}
}  // namespace bcmfm
