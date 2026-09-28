// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The LineageOS Project
#include "BcmFmRadio.h"
#include <jni.h>
#include <android/log.h>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {
class SocketBus final : public bcmfm::Bus {
public:
    bool open() override {
        close();
        fd_ = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (fd_ < 0) return false;
        timeval timeout{2, 0};
        if (setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
            setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
            close();
            return false;
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        memcpy(address.sun_path + 1, bcmfm::kSocket, sizeof(bcmfm::kSocket) - 1);
        const socklen_t length = offsetof(sockaddr_un, sun_path) + sizeof(bcmfm::kSocket);
        ucred peer{};
        socklen_t peerLength = sizeof(peer);
        if (connect(fd_, reinterpret_cast<sockaddr*>(&address), length) != 0 ||
            getsockopt(fd_, SOL_SOCKET, SO_PEERCRED, &peer, &peerLength) != 0 ||
            peerLength != sizeof(peer) || peer.uid != bcmfm::kServerUid) {
            __android_log_print(ANDROID_LOG_ERROR, "BcmFm", "FM endpoint unavailable/untrusted");
            close();
            return false;
        }
        return true;
    }
    void close() override {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }
    bool exchange(const bcmfm::Bytes& request, bcmfm::Bytes* response) override {
        if (fd_ < 0 || !bcmfm::allowed(request.data(), request.size())) return false;
        ssize_t n;
        do { n = send(fd_, request.data(), request.size(), MSG_NOSIGNAL); } while (n < 0 && errno == EINTR);
        if (n != static_cast<ssize_t>(request.size())) {
            __android_log_print(ANDROID_LOG_ERROR, "BcmFm", "FM endpoint send failed: %s", strerror(errno));
            close(); return false;
        }
        uint8_t buffer[bcmfm::kMaxEvent];
        do { n = recv(fd_, buffer, sizeof(buffer), MSG_TRUNC); } while (n < 0 && errno == EINTR);
        if (n <= 0 || static_cast<size_t>(n) > sizeof(buffer)) {
            __android_log_print(ANDROID_LOG_ERROR, "BcmFm", "FM endpoint closed/timed out, received=%zd", n);
            close(); return false;
        }
        response->assign(buffer, buffer + n);
        bcmfm::Bytes data;
        if (!bcmfm::response(request, *response, &data)) {
            __android_log_print(ANDROID_LOG_ERROR, "BcmFm",
                    "Incompatible/error FM completion: reg=%02x size=%zd status=%02x echo=%02x/%02x",
                    request[3], n, n >= 6 ? buffer[5] : 0xff,
                    n >= 7 ? buffer[6] : 0xff, n >= 8 ? buffer[7] : 0xff);
            close(); return false;
        }
        return true;
    }
private:
    int fd_ = -1;
};
class SteadyClock final : public bcmfm::Clock {
public:
    int64_t nowMs() override {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    void sleepMs(int ms) override { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
};
SocketBus bus;
SteadyClock clockSource;
bcmfm::Radio radio(bus, clockSource);
int khz(jfloat mhz) {
    if (!std::isfinite(mhz) || mhz < 87.5f || mhz > 108.0f) return -1;
    const int value = std::lround(mhz * 1000.0);
    return bcmfm::validFrequency(value) ? value : -1;
}
}  // namespace

// The board-specific soname avoids colliding with either the MTK or V4L2 libfmjni.
#define JNI_METHOD(name) Java_com_android_fmradio_FmNative_##name
extern "C" {
JNIEXPORT jboolean JNICALL JNI_METHOD(openDev)(JNIEnv*, jclass) { return JNI_TRUE; }
JNIEXPORT jboolean JNICALL JNI_METHOD(closeDev)(JNIEnv*, jclass) { radio.powerDown(); return JNI_TRUE; }
JNIEXPORT jboolean JNICALL JNI_METHOD(powerUp)(JNIEnv*, jclass, jfloat frequency) { return radio.powerUp(khz(frequency)); }
JNIEXPORT jboolean JNICALL JNI_METHOD(powerDown)(JNIEnv*, jclass, jint type) {
    return type == 0 && radio.powerDown();
}
JNIEXPORT jboolean JNICALL JNI_METHOD(tune)(JNIEnv*, jclass, jfloat frequency) { return radio.tune(khz(frequency)); }
JNIEXPORT jfloat JNICALL JNI_METHOD(seek)(JNIEnv*, jclass, jfloat frequency, jboolean up) {
    int value = radio.seek(khz(frequency), up);
    return value < 0 ? -1.0f : value / 1000.0f;
}
JNIEXPORT jshortArray JNICALL JNI_METHOD(autoScan)(JNIEnv* env, jclass) {
    auto stations = radio.scan();
    if (stations.empty()) return nullptr;
    std::vector<jshort> result;
    for (int value : stations) result.push_back(value / 100); // FMRadio units: 0.1 MHz
    jshortArray array = env->NewShortArray(result.size());
    if (array) env->SetShortArrayRegion(array, 0, result.size(), result.data());
    return array;
}
JNIEXPORT jboolean JNICALL JNI_METHOD(stopScan)(JNIEnv*, jclass) { radio.cancel(); return JNI_TRUE; }
JNIEXPORT jint JNICALL JNI_METHOD(setMute)(JNIEnv*, jclass, jboolean mute) { return radio.mute(mute) ? 1 : -1; }
JNIEXPORT jboolean JNICALL JNI_METHOD(setBcmVolume)(JNIEnv*, jclass, jint volume) { return radio.volume(volume); }
JNIEXPORT jboolean JNICALL JNI_METHOD(isBcmPowered)(JNIEnv*, jclass) { return radio.alive(); }
JNIEXPORT jint JNICALL JNI_METHOD(isRdsSupport)(JNIEnv*, jclass) { return 1; }
JNIEXPORT jint JNICALL JNI_METHOD(setRds)(JNIEnv*, jclass, jboolean enabled) { return radio.setRds(enabled) ? 0 : -1; }
JNIEXPORT jshort JNICALL JNI_METHOD(readRds)(JNIEnv*, jclass) { return radio.pollRds(); }
JNIEXPORT jbyteArray JNICALL JNI_METHOD(getPs)(JNIEnv* env, jclass) {
    const auto value = radio.programService();
    jbyteArray result = env->NewByteArray(value.size());
    if (result) env->SetByteArrayRegion(result, 0, value.size(), reinterpret_cast<const jbyte*>(value.data()));
    return result;
}
JNIEXPORT jbyteArray JNICALL JNI_METHOD(getLrText)(JNIEnv* env, jclass) {
    const auto value = radio.radioText();
    jbyteArray result = env->NewByteArray(value.size());
    if (result) env->SetByteArrayRegion(result, 0, value.size(), reinterpret_cast<const jbyte*>(value.data()));
    return result;
}
JNIEXPORT jshort JNICALL JNI_METHOD(activeAf)(JNIEnv*, jclass) {
    const int frequency = radio.activeAf();
    return frequency < 0 ? -1 : frequency / 100;
}
JNIEXPORT jint JNICALL JNI_METHOD(switchAntenna)(JNIEnv*, jclass, jint antenna) { return antenna == 0 ? 0 : 2; }
}
