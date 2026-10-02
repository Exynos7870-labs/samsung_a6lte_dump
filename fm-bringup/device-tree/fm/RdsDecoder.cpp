// SPDX-License-Identifier: Apache-2.0
#include "RdsDecoder.h"
#include "RdsCharset.h"
#include "BcmFmProtocol.h"
#include <algorithm>

namespace bcmfm {
void RdsDecoder::reset() { *this = RdsDecoder{}; }
void RdsDecoder::clearStation() {
    if (!ps_.empty()) events_ |= kPs;
    if (!text_.empty()) events_ |= kRt;
    ps_.clear(); text_.clear(); af_.clear();
    psSeen_.fill(0); rtSeen_.fill(0); textMode_ = -1;
    afPairHits_.fill(0);
    afCount_ = 0; skipLfMf_ = false; afWork_.clear(); afPrevious_.clear();
}
std::string RdsDecoder::utf8(const uint8_t* bytes, size_t length) {
    std::string result;
    for (size_t n = 0; n < length && bytes[n] != 0x0d; ++n) {
        // Do not emit control characters into notifications/database fields.
        result += bytes[n] < 0x20 ? " " : kRdsChars[bytes[n]];
    }
    while (!result.empty() && result.back() == ' ') result.pop_back();
    return result;
}
bool RdsDecoder::feed(const std::vector<uint8_t>& fifo, int tunedKHz, int64_t nowMs) {
    if (fifo.size() > 240 || fifo.size() % 3 != 0) { next_ = 0; return false; }
    for (size_t n = 0; n < fifo.size(); n += 3) {
        const uint8_t status = fifo[n];
        // Empty/end marker is not an A block and is legal even as the first tuple.
        if (status == 0x7c && fifo[n+1] == 0xff && fifo[n+2] == 0xff) break;
        unsigned type = status >> 4;
        const uint16_t value = (uint16_t(fifo[n+1]) << 8) | fifo[n+2];
        if (((status >> 2) & 3) == 3 || type > 4) { next_ = 0; continue; }
        if (type == 0) { blocks_[0] = value; next_ = 1; continue; }
        if (next_ == 2 && type == 4 && versionB_) type = 2; // C-prime
        else if (next_ == 2 && type == 2 && versionB_) { next_ = 0; continue; }
        if (type != next_ || next_ == 0 || next_ > 3) { next_ = 0; continue; }
        if (type == 1) versionB_ = value & 0x0800;
        blocks_[next_++] = value;
        if (next_ == 4) {
            next_ = 0;
            if (!versionB_ || blocks_[2] == blocks_[0]) group(tunedKHz, nowMs);
        }
    }
    return true;
}
void RdsDecoder::expire(int64_t nowMs) {
    if (lastGroupMs_ >= 0 && nowMs - lastGroupMs_ >= 15000) {
        clearStation(); pi_ = candidatePi_ = 0; piHits_ = next_ = 0; lastGroupMs_ = -1;
    }
}
void RdsDecoder::group(int tunedKHz, int64_t nowMs) {
    if (blocks_[0] == 0) return;
    if (candidatePi_ != blocks_[0]) { candidatePi_ = blocks_[0]; piHits_ = 0; }
    if (piHits_ < 2) ++piHits_;
    // Require a repeated PI before trusting any metadata or AF identity.
    if (piHits_ < 2) return;
    if (pi_ != candidatePi_) { clearStation(); pi_ = candidatePi_; }
    lastGroupMs_ = nowMs;
    const uint16_t b = blocks_[1], c = blocks_[2], d = blocks_[3];
    const unsigned type = b >> 12;
    uint8_t data[4] = {uint8_t(c >> 8), uint8_t(c), uint8_t(d >> 8), uint8_t(d)};
    if (type == 0) {
        segment(false, b & 3, data + 2, 2);
        if (!versionB_) {
            // Method B repeats pairs containing the tuned transmitter. Confirm
            // each pair twice; do not interpret LF/MF escapes or EON groups.
            if (!afCount_ && !skipLfMf_ && data[0] >= 1 && data[0] <= 204 &&
                data[1] >= 1 && data[1] <= 204) {
                const int f0 = 87500 + data[0] * 100, f1 = 87500 + data[1] * 100;
                const int code = f0 == tunedKHz ? data[1] : (f1 == tunedKHz ? data[0] : 0);
                if (code && 87500 + code * 100 != tunedKHz) {
                    if (afPairHits_[code] < 2) ++afPairHits_[code];
                    if (afPairHits_[code] == 2) {
                        if (af_.empty()) af_.push_back(tunedKHz);
                        const int frequency = 87500 + code * 100;
                        if (af_.size() < 25 && std::find(af_.begin(), af_.end(), frequency) == af_.end()) {
                            af_.push_back(frequency); std::sort(af_.begin(), af_.end());
                        }
                    }
                }
            }
            afByte(data[0]); afByte(data[1]);
        }
        // Only confirmed lists/pairs containing this transmitter are eligible. Never treat another network's EON list as an AF list.
        if (!af_.empty() && std::find(af_.begin(), af_.end(), tunedKHz) == af_.end()) af_.clear();
    } else if (type == 2) {
        const int mode = ((b >> 4) & 1) | (versionB_ ? 2 : 0);
        if (mode != textMode_) {
            rtSeen_.fill(0); rtBytes_.fill(0); textMode_ = mode;
            if (!text_.empty()) { text_.clear(); events_ |= kRt; }
        }
        segment(true, b & 15, versionB_ ? data + 2 : data, versionB_ ? 2 : 4);
    }
}
void RdsDecoder::segment(bool rt, unsigned index, const uint8_t* data, unsigned count) {
    uint8_t* bytes = rt ? rtBytes_.data() : psBytes_.data();
    uint8_t* seen = rt ? rtSeen_.data() : psSeen_.data();
    const unsigned segments = rt ? 16 : 4;
    if (index >= segments) return;
    const unsigned offset = index * count;
    if (!std::equal(data, data + count, bytes + offset)) {
        // PS has no A/B flag; discard the partial cycle on a changed segment to
        // avoid stitching a scrolling PS or corrupt block onto the old name.
        if (seen[index]) std::fill(seen, seen + segments, 0);
        std::copy(data, data + count, bytes + offset); seen[index] = 1;
    } else if (seen[index] < 2) ++seen[index];
    unsigned length = segments * count;
    if (rt) {
        for (unsigned n = 0; n < length; ++n) {
            if (seen[n / count] && bytes[n] == 0x0d) { length = n; break; }
        }
    }
    // Include the terminator's segment in the confirmation requirement.
    const unsigned needed = std::min(segments, length / count + 1);
    for (unsigned n = 0; n < (rt ? needed : segments); ++n) if (seen[n] < 2) return;
    std::string value = utf8(bytes, length);
    std::string& published = rt ? text_ : ps_;
    if (published != value) { published = std::move(value); events_ |= rt ? kRt : kPs; }
}
void RdsDecoder::afByte(uint8_t code) {
    if (skipLfMf_) { skipLfMf_ = false; return; }
    if (code == 250) { skipLfMf_ = true; return; }
    if (code >= 224 && code <= 249) {
        afCount_ = code - 224; afWork_.clear();
        if (!afCount_) { af_.clear(); afPrevious_.clear(); afPairHits_.fill(0); }
        return;
    }
    if (!afCount_ || code < 1 || code > 204) return;
    const int khz = 87500 + code * 100;
    if (validFrequency(khz) && std::find(afWork_.begin(), afWork_.end(), khz) == afWork_.end())
        afWork_.push_back(khz);
    if (afWork_.size() == afCount_) {
        std::sort(afWork_.begin(), afWork_.end());
        if (afWork_ == afPrevious_) af_ = afWork_;
        afPrevious_ = afWork_; afCount_ = 0;
    }
}
} // namespace bcmfm
