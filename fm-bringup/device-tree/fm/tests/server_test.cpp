// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The LineageOS Project
// Real broker/socket/thread implementation; only Android HCI, audio and peer UID
// boundaries are mocked. Never run this executable on a phone.
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <iostream>
#include <functional>
#include "hci_layer.h"
#include "osi/include/allocator.h"

std::atomic<int> audioStops{0}, transmissions{0}, behavior{0};
std::atomic<bool> authorized{true};
std::mutex callbackMutex;
std::vector<std::thread> callbacks;
int peerOption(int fd, int level, int name, void* value, socklen_t* length) {
    int result = getsockopt(fd, level, name, value, length);
    if (!result && level == SOL_SOCKET && name == SO_PEERCRED)
        static_cast<ucred*>(value)->uid = authorized ? 1000 : 12345;
    return result;
}
#define getsockopt peerOption
#include BCM_FM_SERVER_SOURCE
#undef getsockopt

void transmit(BT_HDR* command, Complete complete, Status status, void* token) {
    ++transmissions;
    const int mode = behavior.load();
    std::lock_guard<std::mutex> lock(callbackMutex);
    callbacks.emplace_back([=] {
        std::this_thread::sleep_for(std::chrono::milliseconds(mode == 1 ? 1750 : 10));
        if (mode == 2) { status(0x0c, command, token); return; }
        const int size = mode == 3 && command->data[3] == 0x80 ? 248 : 8;
        auto* event = static_cast<BT_HDR*>(osi_calloc(sizeof(BT_HDR) + size));
        event->len = size;
        const uint8_t bytes[] = {0x0e, uint8_t(size - 2), 1, 0x15, 0xfc, 0, command->data[3], command->data[4]};
        memcpy(event->data, bytes, sizeof(bytes));
        complete(event, token);  // callback owns event
        osi_free(command);       // HCI owns command on Command Complete
    });
}
const hci_t* hci_layer_get_interface() { static const hci_t hci{transmit}; return &hci; }
void drain() {
    std::vector<std::thread> copy;
    { std::lock_guard<std::mutex> lock(callbackMutex); copy.swap(callbacks); }
    for (auto& t : copy) t.join();
}
int connectClient() {
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    assert(fd >= 0);
    timeval timeout{3, 0};
    assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    memcpy(address.sun_path + 1, bcmfm::kSocket, sizeof(bcmfm::kSocket) - 1);
    assert(connect(fd, reinterpret_cast<sockaddr*>(&address),
                   offsetof(sockaddr_un, sun_path) + sizeof(bcmfm::kSocket)) == 0);
    return fd;
}
void waitFor(const std::function<bool()>& predicate) {
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate() && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(predicate());
}
int request(int fd, const std::vector<uint8_t>& bytes) {
    ssize_t n = send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
    if (n < 0) return -1;
    uint8_t reply[32];
    n = recv(fd, reply, sizeof(reply), 0);
    if (n > 0) assert(n == 8 && reply[0] == 0x0e && reply[5] == 0);
    return n;
}
int main() {
    const std::vector<uint8_t> on{0x15, 0xfc, 3, 0, 0, 1};
    btif_bcm_fm_start();
    btif_bcm_fm_start();  // idempotent, doesn't replace the active socket/thread
    int client = connectClient();
    assert(request(client, on) == 8);
    int other = connectClient();
    assert(request(other, on) <= 0); close(other);
    assert(request(client, on) == 8);  // rejected second client can't shut first down
    int count = transmissions;
    close(client);
    waitFor([&] { return audioStops == 1; });
    assert(transmissions == count + 1); // client death submitted FM OFF
    std::cout << "PASS session exclusivity and client-death shutdown\n";

    authorized = false;
    client = connectClient(); count = transmissions;
    assert(request(client, on) <= 0); close(client);
    assert(transmissions == count);
    authorized = true;
    std::cout << "PASS unauthorized UID rejected before HCI\n";

    client = connectClient(); count = transmissions;
    // Controller reset / arbitrary vendor opcodes must not enter the HCI queue.
    assert(request(client, {3, 12, 0}) <= 0); close(client);
    waitFor([&] { return audioStops == 2; });
    assert(transmissions == count + 1); // only cleanup OFF, never the bad command
    std::cout << "PASS malformed/opcode request rejected\n";
    btif_bcm_fm_stop(); drain();

    btif_bcm_fm_start();
    behavior = 1; client = connectClient();
    assert(request(client, on) <= 0); // 1.5s deadline, later callback still owns packet
    close(client); btif_bcm_fm_stop();
    // New adapter epoch before the previous epoch's delayed completion arrives.
    behavior = 0; btif_bcm_fm_start();
    client = connectClient(); assert(request(client, on) == 8);
    drain();
    assert(request(client, on) == 8);
    close(client); btif_bcm_fm_stop(); drain();
    std::cout << "PASS timeout, late callback and adapter restart\n";

    behavior = 2; btif_bcm_fm_start(); client = connectClient();
    assert(request(client, on) <= 0);
    close(client); btif_bcm_fm_stop(); drain();
    std::cout << "PASS Command Status failure and ownership cleanup\n";

    behavior = 1; btif_bcm_fm_start(); client = connectClient();
    count = transmissions;
    assert(send(client, on.data(), on.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(on.size()));
    waitFor([&] { return transmissions > count; });
    auto start = std::chrono::steady_clock::now();
    btif_bcm_fm_stop();
    assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
    close(client); drain(); btif_bcm_fm_stop();
    std::cout << "PASS adapter shutdown unblocks an outstanding request\n";
    behavior = 3; btif_bcm_fm_start(); client = connectClient();
    const std::vector<uint8_t> fifo{0x15,0xfc,3,0x80,1,240};
    assert(send(client, fifo.data(), fifo.size(), MSG_NOSIGNAL)==6);
    uint8_t packet[257];
    const auto length = recv(client, packet, sizeof(packet), MSG_TRUNC);
    assert(length==248);
    std::vector<uint8_t> data;
    assert(bcmfm::response(fifo, {packet, packet+length}, &data) && data.size()==240);
    count = transmissions;
    assert(request(client, {0x15,0xfc,3,0x80,1,239})<=0); // no arbitrary FIFO/memory length
    close(client); waitFor([&] { return transmissions==count+1; }); drain();
    assert(transmissions==count+1); // only cleanup OFF
    btif_bcm_fm_stop(); drain();
    std::cout << "PASS bounded RDS FIFO socket payload and invalid length rejection\n";

}
