// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The LineageOS Project
#include "BcmFmRadio.h"
#include <algorithm>
#include <cassert>
#include <functional>
#include <iostream>
#include <map>

using namespace bcmfm;
struct FakeClock : Clock {
    int64_t time = 0;
    std::function<void(int)> sleepHook;
    int64_t nowMs() override { return time; }
    void sleepMs(int ms) override { time += ms; if (sleepHook) sleepHook(ms); }
};
struct FakeBus : Bus {
    bool available = true, opened = false, badEcho = false, noCompletion = false;
    int opens = 0, closes = 0, failAt = -1, wrongTune = 0, forcedSeek = 0;
    int flags = 0;
    std::vector<int> stations{88000, 94500, 101100, 108000};
    std::map<int, int> registers;
    std::vector<Bytes> calls;
    std::function<void()> exchangeHook;
    bool open() override { ++opens; return opened = available; }
    void close() override { ++closes; opened = false; }
    bool exchange(const Bytes& request, Bytes* result) override {
        assert(allowed(request.data(), request.size()));
        calls.push_back(request);
        if (exchangeHook) exchangeHook();
        if (!opened || !available || static_cast<int>(calls.size()) == failAt) return false;
        int reg = request[3], mode = request[4];
        *result = {0x0e, 6, 1, 0x15, 0xfc, 0, static_cast<uint8_t>(reg), static_cast<uint8_t>(mode)};
        if (badEcho) (*result)[6] ^= 1;
        if (!mode) {
            registers[reg] = request.size() == 7 ? le16(request.data() + 5) : request[5];
            if (reg == 0x09) {
                flags = 0;
                if (registers[reg] == 1) {
                    flags = 1;
                    if (wrongTune) registers[0x0a] = wrongTune - 64000;
                }
                if (registers[reg] == 2) {
                    int khz = registers[0x0a] + 64000;
                    int found = -1;
                    if (forcedSeek) found = forcedSeek;
                    else if (registers[0x07] & 0x80) {
                        auto station = std::lower_bound(stations.begin(), stations.end(), khz);
                        if (station != stations.end()) found = *station;
                    } else {
                        auto station = std::upper_bound(stations.begin(), stations.end(), khz);
                        if (station != stations.begin()) found = *std::prev(station);
                    }
                    flags = found < 0 ? 2 : 1;
                    if (found >= 0) registers[0x0a] = found - 64000;
                }
            }
        } else {
            int value = reg == 0x12 ? (noCompletion ? 0 : flags) : registers[reg];
            if (reg == 0x12) flags = 0;
            result->push_back(value & 255);
            if (request[5] == 2) result->push_back((value >> 8) & 255);
            (*result)[1] = result->size() - 2;
        }
        return true;
    }
    bool wrote(int reg, int value) const {
        return std::any_of(calls.begin(), calls.end(), [=](const Bytes& b) {
            return b[3] == reg && b[4] == 0 && (b.size() == 7 ? le16(b.data() + 5) : b[5]) == value;
        });
    }
};
struct Fixture {
    FakeClock clock;
    FakeBus bus;
    Radio radio{bus, clock};
};
int main() {
    int tests = 0;
    auto test = [&](const char* name, const std::function<void()>& body) {
        body(); ++tests; std::cout << "PASS " << name << '\n';
    };
    test("EU boundaries and grid", [] {
        assert(validFrequency(87500)); assert(validFrequency(108000));
        for (int f : {-1, 0, 76000, 87499, 87550, 108100, 2147483647}) assert(!validFrequency(f));
    });
    test("HCI allowlist and framing", [] {
        const Bytes ok{0x15, 0xfc, 3, 0x00, 0, 1};
        assert(allowed(ok.data(), ok.size()));
        for (size_t n = 0; n < ok.size(); ++n) assert(!allowed(ok.data(), n));
        for (size_t i : {0U, 1U, 2U, 4U, 5U}) {
            auto bad = ok; bad[i] = 0xff; assert(!allowed(bad.data(), bad.size()));
        }
        for (int reg = 0; reg <= 255; ++reg) {
            Bytes read{0x15, 0xfc, 3, static_cast<uint8_t>(reg), 1, 1};
            bool expected = reg == 0 || reg == 1 || reg == 0x0f || reg == 0x4d || reg == 0xdf;
            assert(allowed(read.data(), read.size()) == expected);
        }
        for (Bytes bad : {Bytes{0x61, 0xfc, 5, 7, 0x19, 0x18, 0x19, 0x19},
                         Bytes{0x15, 0xfc, 4, 0xf8, 0, 0, 1},
                         Bytes{0x15, 0xfc, 3, 0x00, 0, 3},
                         Bytes{0x15, 0xfc, 3, 0x4d, 0, 0x80},
                         Bytes{0x15, 0xfc, 3, 0x80, 1, 128}})
            assert(!allowed(bad.data(), bad.size()));
    });
    test("completion validation", [] {
        Bytes req{0x15, 0xfc, 3, 0x12, 1, 2};
        Bytes evt{0x0e, 8, 1, 0x15, 0xfc, 0, 0x12, 1, 1, 0}, data;
        assert(response(req, evt, &data) && data == Bytes({1, 0}));
        for (size_t i : {0U, 1U, 3U, 4U, 5U, 6U, 7U}) {
            auto bad = evt; bad[i] ^= 1; assert(!response(req, bad, &data));
        }
        for (size_t n = 0; n < evt.size(); ++n) {
            auto shortEvent = Bytes(evt.begin(), evt.begin() + n);
            assert(!response(req, shortEvent, &data));
        }
        evt.push_back(0); evt[1]++; assert(!response(req, evt, &data));
    });
    test("power on sequence and safe initial volume", [] {
        Fixture f; assert(f.radio.powerUp(100000));
        assert(f.bus.registers[0] == 1 && f.clock.time == 300);
        assert(f.bus.wrote(0xf8, 0) && f.bus.wrote(0x05, 0x2f));
        assert(f.bus.wrote(0x01, 2) && f.bus.wrote(0xfd, 1));
        assert(f.bus.registers[0x0a] + 64000 == 100000);
        assert(f.radio.alive());
        assert(f.radio.powerUp(94500) && f.bus.opens == 1);
        f.bus.failAt = f.bus.calls.size() + 1;
        assert(!f.radio.powerUp(101100));
        assert(f.bus.registers[0x0a] + 64000 == 94500);
    });
    test("invalid input never powers or tunes hardware", [] {
        Fixture f;
        assert(!f.radio.powerUp(87550) && f.bus.calls.empty());
        assert(!f.radio.tune(100000) && f.bus.calls.empty());
        assert(f.radio.seek(100000, true) == -1 && f.bus.calls.empty());
    });
    test("unavailable Bluetooth endpoint", [] {
        Fixture f; f.bus.available = false;
        assert(!f.radio.powerUp(100000)); assert(f.bus.calls.empty());
    });
    test("power-down idempotence", [] {
        Fixture f; assert(f.radio.powerDown()); assert(f.bus.closes == 0);
        assert(f.radio.powerUp(100000)); assert(f.radio.powerDown());
        assert(f.bus.wrote(0, 0) && f.bus.closes == 1);
        assert(f.radio.powerDown()); assert(f.bus.closes == 1);
        assert(!f.radio.alive());
    });
    test("every power-on I/O failure rolls back", [] {
        Fixture good; assert(good.radio.powerUp(100000));
        const int operations = good.bus.calls.size();
        for (int i = 1; i <= operations; ++i) {
            Fixture f; f.bus.failAt = i;
            assert(!f.radio.powerUp(100000));
            assert(!f.bus.opened && f.bus.wrote(0, 0));
            assert(!f.radio.alive());
        }
    });
    test("bad echo fails closed", [] {
        Fixture f; f.bus.badEcho = true;
        assert(!f.radio.powerUp(100000)); assert(!f.bus.opened && f.bus.wrote(0, 0));
    });
    test("does not steal FM-over-SCO", [] {
        Fixture f; f.bus.registers[0x4d] = 0x80;
        assert(!f.radio.powerUp(100000)); assert(f.bus.registers[0x4d] == 0x80);
    });
    test("mute and digital volume bounds", [] {
        Fixture f; assert(f.radio.powerUp(100000));
        assert(f.radio.mute(false) && f.bus.registers[5] == 0x2d);
        assert(f.radio.mute(true) && f.bus.registers[5] == 0x2f);
        assert(f.radio.volume(0)); assert(f.radio.volume(128)); assert(f.radio.volume(255));
        size_t n = f.bus.calls.size();
        assert(!f.radio.volume(-1) && !f.radio.volume(256)); assert(f.bus.calls.size() == n);
    });
    test("tune and readback", [] {
        Fixture f; assert(f.radio.powerUp(100000)); assert(f.radio.tune(94500));
        assert(f.bus.registers[0x0a] == 30500);
        assert(!f.radio.tune(94550)); assert(f.bus.registers[0x0a] == 30500);
    });
    test("mismatched tune readback never succeeds", [] {
        Fixture f; assert(f.radio.powerUp(100000)); f.bus.wrongTune = 101100;
        assert(!f.radio.tune(94500)); assert(!f.radio.alive());
    });
    test("seek both directions excludes current channel", [] {
        Fixture f; assert(f.radio.powerUp(94500));
        assert(f.radio.seek(94500, true) == 101100);
        assert(f.radio.seek(101100, false) == 94500);
    });
    test("seek wraps at band boundaries", [] {
        Fixture f; assert(f.radio.powerUp(108000));
        assert(f.radio.seek(108000, true) == 88000);
        assert(f.radio.seek(87500, false) == 108000);
    });
    test("seek wraps only on explicit band-limit response", [] {
        Fixture f; f.bus.stations = {88000}; assert(f.radio.powerUp(100000));
        assert(f.radio.seek(100000, true) == 88000);
    });
    test("no station returns failure and restores tune", [] {
        Fixture f; assert(f.radio.powerUp(100000)); f.bus.stations.clear();
        assert(f.radio.seek(100000, true) == -1);
        assert(f.bus.registers[0x0a] + 64000 == 100000);
    });
    test("scan sorted and restores original station", [] {
        Fixture f; assert(f.radio.powerUp(100000));
        assert(f.radio.scan() == f.bus.stations);
        assert(f.bus.registers[0x0a] + 64000 == 100000);
    });
    test("implicit firmware seek wrap cannot loop forever", [] {
        Fixture f; assert(f.radio.powerUp(100000)); f.bus.forcedSeek = 88000;
        assert(f.radio.scan() == std::vector<int>{88000});
        assert(f.clock.time < 1000);
    });
    test("power-up cancellation rolls back", [] {
        Fixture f; f.clock.sleepHook = [&](int) { f.radio.cancel(); };
        assert(!f.radio.powerUp(100000)); assert(f.bus.wrote(0, 0) && !f.bus.opened);
        for (size_t at = 1; at <= 11; ++at) {
            Fixture mid;
            mid.bus.exchangeHook = [&] { if (mid.bus.calls.size() == at) mid.radio.cancel(); };
            assert(!mid.radio.powerUp(100000));
            assert(mid.bus.calls.size() == at + 2); // only mute and OFF after cancellation
            assert(mid.bus.registers[0] == 0 && !mid.bus.opened);
        }
    });
    test("seek cancellation interrupts the poll and restores", [] {
        Fixture f; assert(f.radio.powerUp(100000)); f.bus.noCompletion = true;
        f.clock.sleepHook = [&](int) { f.radio.cancel(); f.bus.noCompletion = false; };
        assert(f.radio.seek(100000, true) == -1);
        assert(f.bus.registers[0x0a] + 64000 == 100000);
        assert(f.clock.time < 1000);
    });
    test("scan cancellation discards partial stations", [] {
        Fixture f; assert(f.radio.powerUp(100000)); f.bus.noCompletion = true;
        f.clock.sleepHook = [&](int) { f.radio.cancel(); f.bus.noCompletion = false; };
        assert(f.radio.scan().empty()); assert(f.clock.time < 1000);
    });
    test("timeout is bounded and powers down if restoration also fails", [] {
        Fixture f; assert(f.radio.powerUp(100000)); f.bus.noCompletion = true;
        assert(f.radio.seek(100000, true) == -1);
        assert(f.clock.time <= 22500); assert(!f.radio.alive());
    });
    test("link loss health check clears local state", [] {
        Fixture f; assert(f.radio.powerUp(100000)); f.bus.available = false;
        assert(!f.radio.alive()); assert(!f.bus.opened);
        assert(!f.radio.tune(94500));
    });
    std::cout << tests << " native tests passed\n";
}
