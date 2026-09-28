// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bcmfm {
// Decoder for the Broadcom FIFO tuples [status, MSB, LSB], not raw RF bits.
// Firmware corrects CRC; reject uncorrectable blocks and broken group ordering.
class RdsDecoder {
public:
    static constexpr int kPs = 0x0008, kRt = 0x0040, kAf = 0x0080;
    void reset();
    bool feed(const std::vector<uint8_t>& fifo, int tunedKHz, int64_t nowMs);
    void expire(int64_t nowMs);
    int takeEvents() { int result = events_; events_ = 0; return result; }
    uint16_t pi() const { return pi_; }
    const std::string& ps() const { return ps_; }
    const std::string& text() const { return text_; }
    const std::vector<int>& alternatives() const { return af_; }
    static std::string utf8(const uint8_t* text, size_t length);
private:
    void group(int tunedKHz, int64_t nowMs);
    void clearStation();
    void afByte(uint8_t code);
    void segment(bool rt, unsigned index, const uint8_t* data, unsigned count);
    std::array<uint16_t, 4> blocks_{};
    unsigned next_ = 0;
    uint16_t pi_ = 0, candidatePi_ = 0;
    unsigned piHits_ = 0;
    bool versionB_ = false;
    int events_ = 0;
    int64_t lastGroupMs_ = -1;
    std::string ps_, text_;
    std::array<uint8_t, 64> rtBytes_{};
    std::array<uint8_t, 8> psBytes_{};
    std::array<uint8_t, 16> rtSeen_{};
    std::array<uint8_t, 4> psSeen_{};
    int textMode_ = -1;
    unsigned afCount_ = 0;
    bool skipLfMf_ = false;
    std::vector<int> afWork_, afPrevious_, af_;
    std::array<uint8_t, 205> afPairHits_{};
};
} // namespace bcmfm
