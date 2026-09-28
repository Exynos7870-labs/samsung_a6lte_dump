# A6 (a6lte) FM radio bring-up — LineageOS 18.1

## Status

A source-based, hardware-unverified integration candidate is now implemented.
See **[fm-integration.md](fm-integration.md)** for the current architecture, build
instructions, supported paths and limitations. This page preserves the stock
findings and optional capture workflow. A stock log is **not a prerequisite** for
building or trying the candidate. Neither host tests nor binary inspection prove
that reception or audio works on an A6.

## Source selection

The intended source layout is the organization's
[j5y17lte lineage-18.1-lts manifest](https://github.com/samsungexynos7870/android_manifest_samsung_j5y17lte/blob/lineage-18.1-lts/j5y17lte.xml),
with the device-specific projects replaced by A6 projects:

| Path | GitHub repository (under samsungexynos7870 unless noted) | Revision |
| --- | --- | --- |
| `device/samsung/a6lte` | `android_device_samsung_a6lte` | `lineage-18.1` |
| `device/samsung/universal7870-common` | `android_device_samsung_universal7870-common` | `lineage-18.1-oss_bsp-vndk` |
| `kernel/samsung/exynos7870` | `android_kernel_samsung_exynos7870` | `lineage-18.1-lts` |
| `vendor/samsung/a6lte` | `android_vendor_samsung_a6lte` | `lineage-18.1` |
| `vendor/samsung/universal7870-common` | `android_vendor_samsung_universal7870-common` | `lineage-18.1-lts` |
| `hardware/samsung` | `android_hardware_samsung` | `lineage-18.1` |
| `system/bt` | `LineageOS/android_system_bt` | `lineage-18.1` |
| `packages/apps/FMRadio` | `LineageOS/android_packages_apps_FMRadio` | `lineage-18.1` |

Keep the remaining BSP and policy projects at the manifest's revisions. In
particular, the common device tree is **not** its plain `lineage-18.1` branch.
The latter requests a different primary audio module. The matching LTS vendor
branch has the Samsung SEC primary audio HAL used by the A6 device's existing
`TARGET_DEVICE_HAS_SEC_AUDIO_HAL := true` selection.

The device-tree patch base is `a6b97426994a2d03df18087ed8b8c4c84a69905c`.
The inspected common/kernel/vendor-common commits are respectively
`a433c37b08659956b366e06019c36e310a0b2781`,
`e41078f882271fe43089a683a200bdab8803483b`, and
`5c77cae3529a5ca03f12a5988a7cb0247bbced96`.
These are inspection baselines, not evidence of a successfully built ROM.

## Evidence from stock A600FNXXU9CVB1

The reference dump is
[Exynos7870-labs/samsung_a6lte_dump](https://github.com/Exynos7870-labs/samsung_a6lte_dump/tree/59c75738f83f4307f18f41b80cf2b578dad9ff51),
fingerprint
`samsung/a6ltexx/a6lte:10/QP1A.190711.020/A600FNXXU9CVB1:user/release-keys`.

* The embedded stock `boot/kernel` IKCONFIG contains `CONFIG_BT_BCM43XX=y`,
  `CONFIG_BCM43456=y`, `# CONFIG_MEDIA_RADIO_SUPPORT is not set`, and
  `# CONFIG_FM_SI47XX is not set`.
* `vendor/etc/floating_feature.xml` reports FM chip-vendor value `2`. This value
  alone is not used to identify the chip: the kernel and Bluetooth libraries
  corroborate a Broadcom path.
* `system/system/lib/libbluetooth.so` contains FM-specific strings such as
  `get_fm_interface`, `btif_fm_enable`/`btif_fm_disable`, and `BTA_FM_*` events.
  `libbluetooth_jni.so` contains `register_com_broadcom_fm_service` and native FM
  callback names. The stock FM support is integrated into Samsung's Bluetooth
  stack, not a standalone FM JNI library ready to copy into LineageOS.
* Stock and the A6 blob list supply
  `/vendor/firmware/bcm4345C5_V0069.0172.hcd`. The pre-existing device config
  instead named `bcm43455.hcd`. The firmware-selection patch selects the file
  actually shipped. It does not replace firmware bytes or prove they expose a
  compatible FM command protocol under LineageOS.
* The device mixer files already contain `fm_radio-headset`, `fm_radio-speaker`,
  and `fm_radio-fm-recording`, their gain paths, and an FM PCM DAI link of `4`.
* Stock's audio HAL recognizes `l_fmradio_mode`. Inspection of the stock ARM
  code identifies comparisons with `ready` and `on`; this is **not** enough to
  determine the full start/stop/routing/volume lifecycle.
* Generic init grants to `/dev/radio0` exist even in stock, whose kernel has no
  V4L2 radio support enabled. Those grants do not establish that a tuner device
  exists. Likewise the common audio policy's FM input port does not prove audio
  capture or playback is operational.

Do **not** enable Si470x/RTC6213N support based on `/dev/radio0` init entries.
Do **not** simply set `BOARD_HAVE_BCM_FM` and ship `FMRadio`: LineageOS's
`hardware/broadcom/fm` backend requires a kernel V4L2/shared-transport driver that
is not provided by the inspected A6 kernel. Importing that backend entails a
separate UART/line-discipline port; it is not an app-only change.
`libfmq.so` is a Fast Message Queue library, not an FM receiver implementation.
Samsung's `HybridRadio.apk` also depends on Samsung framework/Bluetooth APIs;
copying the APK alone is not an integration.

## Optional follow-up: a stock receive session

Use a stock **A6**, not a J5 or another BCM device. A trace can verify firmware register definitions,
I2S pin routing, and power sequencing against this board. The current candidate
uses documented protocol behavior and existing board configuration in its absence.

1. Connect wired headphones (the antenna). In Developer options enable full
   Bluetooth HCI snoop logging if available. Restart Bluetooth as required by
   the stock logging setting. Do not change UART ownership or line discipline.
2. Start from FM off. Note whether Bluetooth was initially off or on. Open stock
   Radio, power on, tune a known local station, seek once, switch to speaker and
   back, change volume, and power off. Note station frequencies and the action
   order. Repeat once with the opposite initial Bluetooth state if practical.
3. Retrieve the HCI log using the device's supported mechanism. On some builds
   `adb bugreport fm-stock.zip` includes a btsnoop file; on others a rooted stock
   device is needed to read it. Paths vary, so inspect archive member names
   rather than assuming `/sdcard/btsnoop_hci.log` exists. Do not root or flash a
   working phone just for this task without weighing the consequences.
4. Extract the uncompressed `btsnoop_hci.log` locally. The decoder accepts
   btsnoop v1 H4/datalink 1002, **not** a ZIP or `btsnooz` container:

   ```sh
   python3 device/samsung/a6lte/tools/fm/decode_btsnoop.py \
       /path/to/btsnoop_hci.log > fm-stock.jsonl
   ```

5. Check the exit code. `0` means selected records were decoded, `1` means none
   were present, and `2` means invalid/unreadable input. Discard partial output
   on exit `2`. Review the JSONL before sharing it, along with the action order,
   phone model, and stock build. Do not share the whole capture or bugreport:
   they can contain device addresses, pairing information, and personal data.
6. Disable HCI logging afterward. A trace with no selected records is not proof
   of no FM support: stock may filter vendor commands. In that case report the
   logging mode, file format, and file size rather than uploading all traffic.

The decoder keeps only opcode `0xfc15` register transactions, `0xfc61` PCM pin
transactions, their completion/status responses, and vendor subevent `0x08`.
It preserves raw parameters, relative timing, errors, and drop counts. Register
labels are hints from the Broadcom FM protocol, not validated BCM4345C5 register
semantics. `0xfc61` is also used by Bluetooth audio; its presence alone is not
FM evidence. Filtered output can still reveal station/RDS data and timing.

For audio integration, collect local before/during/after snapshots of
`adb shell dumpsys media.audio_flinger` and `adb shell dumpsys media.audio_policy`.
Review for personal information before sharing relevant sections. These may
identify PCM/routing state but do not replace tracing the Samsung audio
parameter sequence. Do not infer that a string found in the HAL is sufficient
to enable audio.

## Implementation and remaining validation

The Bluetooth-owned command endpoint, FMRadio JNI backend, device selection,
app lifecycle, direct SEC audio path and narrow IPC policy are implemented in
the companion patch series. No kernel radio driver or stock APK transplant is
used. See [fm-integration.md](fm-integration.md) for implementation details.

Remaining work is an Android/SELinux build, phone validation of the inferred
protocol and audio sequence, and implementation/validation of the deliberately
disabled RDS/AF and recording features. Regional modes beyond the EU band,
secondary users, and FM-to-Bluetooth audio are not supported in this candidate.
Normal Bluetooth/Wi-Fi coexistence also needs hardware testing. A stock trace
can refine the implementation later; new-ROM failure logs can be used first.

## Host tests

From this device repository:

```sh
python3 -m unittest discover -s tools/fm/tests -v
```

Tests cover framing, filtering, raw parameter preservation, statuses,
interrupts, relative timestamps, malformed/truncated records, and CLI failures.
No Python packages outside the standard library are required.
