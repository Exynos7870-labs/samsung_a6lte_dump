// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The LineageOS Project
#define LOG_TAG "bt_bcm_fm"
#include "bt_target.h"
#include "btif_bcm_fm.h"

#if defined(BRCM_FM_HCI_INCLUDED) && (BRCM_FM_HCI_INCLUDED == TRUE)
#include "BcmFmProtocol.h"
#include "hci_layer.h"
#include "osi/include/allocator.h"
#include "osi/include/log.h"
#include <media/AudioSystem.h>
#include <utils/String8.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using Bytes = std::vector<uint8_t>;
std::mutex lifecycle;
struct State {
    std::mutex mutex;
    std::condition_variable ready;
    bool running = false;
    bool healthy = true;
    bool pending = false;
    uintptr_t serial = 0;  // never reset between adapter lifetimes
    Bytes reply;
    int listener = -1;
    int wake = -1;
    std::thread worker;
} state;

void deliver(void* token, Bytes reply) {
    std::lock_guard<std::mutex> lock(state.mutex);
    // No request-owned pointer: late callbacks after timeout/shutdown cannot
    // dereference freed memory or complete a newer adapter/session's request.
    if (state.pending && state.serial == reinterpret_cast<uintptr_t>(token)) {
        state.reply = std::move(reply);
        state.pending = false;
        state.ready.notify_all();
    }
}
void complete(BT_HDR* packet, void* token) {
    Bytes bytes;
    if (packet->len <= bcmfm::kMaxEvent) {
        const uint8_t* data = packet->data + packet->offset;
        bytes.assign(data, data + packet->len);
    }
    osi_free(packet);  // HCI gives ownership of Command Complete to the callback
    deliver(token, std::move(bytes));
}
void status(uint8_t code, BT_HDR* command, void* token) {
    // Register VSCs must complete, not merely be accepted for async execution.
    LOG_ERROR(LOG_TAG, "FM VSC returned Command Status 0x%02x", code);
    osi_free(command);  // ownership specified by hci_layer.h/filter_incoming_event
    deliver(token, {});
}

bool exchange(const Bytes& request, Bytes* reply) {
    std::unique_lock<std::mutex> lock(state.mutex);
    if (!state.running || !state.healthy) return false;
    const hci_t* hci = hci_layer_get_interface();
    if (!hci || !hci->transmit_command) return false;
    state.pending = true;
    state.reply.clear();
    if (++state.serial == 0) ++state.serial;
    BT_HDR* command = static_cast<BT_HDR*>(osi_calloc(sizeof(BT_HDR) + request.size()));
    command->len = static_cast<uint16_t>(request.size());
    memcpy(command->data, request.data(), request.size());
    // transmit_command enqueues asynchronously and preserves HCI command credits
    // and event ownership. Never send via the vendor HAL or UART in parallel.
    hci->transmit_command(command, complete, status, reinterpret_cast<void*>(state.serial));
    if (!state.ready.wait_for(lock, std::chrono::milliseconds(1500), [] {
            return !state.pending || !state.running;
        }) || !state.running || state.reply.empty()) {
        state.pending = false;
        state.healthy = false;  // don't retry queued commands on an ambiguous link
        LOG_ERROR(LOG_TAG, "FM transport unavailable/timed out; toggle Bluetooth to recover");
        return false;
    }
    *reply = std::move(state.reply);
    return true;
}

void cleanup_session() {
    Bytes ignored;
    // Client death must not leave the receiver running. On adapter shutdown the
    // controller will be powered down; exchange() then deliberately refuses work.
    exchange({0x15, 0xfc, 3, 0x00, 0x00, 0x00}, &ignored);
    // FMRadio uses direct SEC-HAL playback, not a software loopback. Reset it on
    // client death too, after AudioFlinger has lost the app's keep-alive track.
    android::AudioSystem::setParameters(0, android::String8("l_fmradio_mode=off"));
}

void serve() {
    int client = -1;
    while (true) {
        pollfd fds[] = {{state.wake, POLLIN, 0}, {state.listener, POLLIN, 0},
                        {client, POLLIN, 0}};
        int result;
        do { result = poll(fds, 3, -1); } while (result < 0 && errno == EINTR);
        if (result < 0 || fds[0].revents) break;
        if (fds[1].revents & POLLIN) {
            int fd = accept4(state.listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (fd >= 0) {
                ucred peer{};
                socklen_t length = sizeof(peer);
                if (client >= 0 || getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &length) != 0 ||
                    length != sizeof(peer) || peer.uid != bcmfm::kClientUid) {
                    close(fd);
                } else {
                    client = fd;
                }
            }
        }
        if (fds[2].revents && client >= 0) {
            uint8_t buffer[16];
            ssize_t n = recv(client, buffer, sizeof(buffer), MSG_TRUNC | MSG_DONTWAIT);
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            Bytes reply;
            bool ok = n > 0 && static_cast<size_t>(n) <= sizeof(buffer) &&
                      bcmfm::allowed(buffer, n) && exchange(Bytes(buffer, buffer + n), &reply);
            if (ok) {
                ok = send(client, reply.data(), reply.size(), MSG_NOSIGNAL) ==
                     static_cast<ssize_t>(reply.size());
            }
            if (!ok) {
                close(client);
                client = -1;
                cleanup_session();
            }
        }
    }
    if (client >= 0) {
        close(client);
        cleanup_session();
    }
}
}  // namespace

void btif_bcm_fm_start() {
    std::lock_guard<std::mutex> life(lifecycle);
    if (state.worker.joinable()) return;
    int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    int wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path + 1, bcmfm::kSocket, sizeof(bcmfm::kSocket) - 1);
    const socklen_t length = offsetof(sockaddr_un, sun_path) + sizeof(bcmfm::kSocket);
    if (listener < 0 || wake < 0 || bind(listener, reinterpret_cast<sockaddr*>(&address), length) != 0 ||
        listen(listener, 1) != 0) {
        LOG_ERROR(LOG_TAG, "Cannot start FM endpoint: %s", strerror(errno));
        if (listener >= 0) close(listener);
        if (wake >= 0) close(wake);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.listener = listener;
        state.wake = wake;
        state.running = state.healthy = true;
        state.pending = false;
    }
    state.worker = std::thread(serve);
    LOG_INFO(LOG_TAG, "A6 experimental Broadcom FM endpoint started");
}

void btif_bcm_fm_stop() {
    std::lock_guard<std::mutex> life(lifecycle);
    if (!state.worker.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.running = false;
        state.pending = false;
        state.ready.notify_all();
    }
    uint64_t value = 1;
    ssize_t written;
    do { written = write(state.wake, &value, sizeof(value)); } while (written < 0 && errno == EINTR);
    state.worker.join();
    close(state.listener);
    close(state.wake);
    state.listener = state.wake = -1;
}
#else
void btif_bcm_fm_start() {}
void btif_bcm_fm_stop() {}
#endif
