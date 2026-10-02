// Host-only model of the three hci_layer.h entry points used by the broker.
#pragma once
#include <cstdint>
struct BT_HDR { uint16_t event, len, offset, layer_specific; uint8_t data[]; };
using Complete = void (*)(BT_HDR*, void*);
using Status = void (*)(uint8_t, BT_HDR*, void*);
struct hci_t { void (*transmit_command)(BT_HDR*, Complete, Status, void*); };
const hci_t* hci_layer_get_interface();
