# Experimental A6 Broadcom FM integration

This is a **source-based integration candidate**, not a hardware-validated FM
release. It uses the current A6 vendor repository at
`f0c55d795a9be60147d76a42ec93fd9ba3e5800f` (`lineage-18.1`), the supplied stock dump,
and the Broadcom FM register protocol. A stock log is useful but is **not required
to build or try this candidate**. No vendor blobs or kernel files are changed.
The current vendor's `bcm4345C5_V0069.0172.hcd` is byte-identical to the dump
(SHA-256 `fe5d61771d2e26310f295ee7241b5d188b80d80d3b6805f4a6452d52783a2225`).

Apply the companion patches to **both** `system/bt` and `packages/apps/FMRadio`,
then apply the device patches. Adding only `FMRadio` to PRODUCT_PACKAGES, or
applying only this device patch, is not sufficient.

## Source baselines

| Project | Revision | Inspected base |
| --- | --- | --- |
| device/samsung/a6lte | samsungexynos7870 lineage-18.1 | `a6b97426994a2d03df18087ed8b8c4c84a69905c` |
| vendor/samsung/a6lte | samsungexynos7870 lineage-18.1 | `f0c55d795a9be60147d76a42ec93fd9ba3e5800f` |
| device/samsung/universal7870-common | samsungexynos7870 lineage-18.1-oss_bsp-vndk | `a433c37b08659956b366e06019c36e310a0b2781` |
| vendor/samsung/universal7870-common | samsungexynos7870 lineage-18.1-lts | `5c77cae3529a5ca03f12a5988a7cb0247bbced96` |
| kernel/samsung/exynos7870 | samsungexynos7870 lineage-18.1-lts | `e41078f882271fe43089a683a200bdab8803483b` |
| system/bt | LineageOS lineage-18.1 | `bcca9a572f1cc539ba21ebe940c3642ce8707d1a` |
| packages/apps/FMRadio | LineageOS lineage-18.1 | `c8664126026de37a442dba996db8bc4d3606dc41` |

Use the other BSP/policy projects from the supplied
[j5y17lte LTS manifest](https://github.com/samsungexynos7870/android_manifest_samsung_j5y17lte/blob/lineage-18.1-lts/j5y17lte.xml),
substituting the A6 device and vendor projects. See `fm-radio.md` for stock
hardware evidence. That document records the earlier investigation; the
implementation and current status are described here.

## Architecture

```
platform-signed FMRadio (system UID, main Android user)
    libfmjni_bcm_hci -> authenticated abstract SOCK_SEQPACKET endpoint
        Bluetooth process / btif_bcm_fm
            Fluoride transmit_command + its existing credits/completions
                existing Bluetooth HAL -> existing UART -> BCM4345C5 firmware

FMRadio -> AudioManager l_fmradio_mode -> Samsung SEC audio HAL
    existing FM DAI 4 + fm_radio-{headset,speaker} mixer paths
```

* The controller remains owned by Bluetooth. The FM code never opens the UART,
  loads firmware, changes line discipline, resets the controller, enables
  `/dev/radio0`, or rewrites I2S/PCM pins. Bluetooth must be enabled by the user;
  FM does not silently change that preference or disable Bluetooth on exit.
* The endpoint is created after Bluetooth initialization and stopped before HCI
  teardown. Only UID 1000 can connect; the client checks the server's UID 1002.
  Only one FM session is accepted. The protocol allowlist validates exact
  register widths and values and permits only the needed `0xfc15` operations,
  **not arbitrary vendor commands or `0xfc61` pin control**.
* There is one outstanding FM transaction, using the existing HCI queue.
  Responses, errors, shutdown, late callbacks and 1.5-second timeouts have
  explicit ownership/lifetime handling. A timed-out endpoint requires a
  Bluetooth toggle before retry rather than piling ambiguous retries into HCI.
  Bluetooth's own controller watchdog can still reset Bluetooth if firmware
  hangs on an FM command; this cannot be ruled out without hardware testing.
* On client death the broker attempts FM OFF and resets the SEC audio route.
  On adapter shutdown it does not submit new commands to a tearing-down HCI
  layer; Bluetooth then owns controller power-off.
* The device enables `BOARD_HAVE_BCM_FM_HCI` and `BRCM_FM_HCI_INCLUDED` plus the
  app overlay. Do **not** substitute `BOARD_HAVE_BCM_FM`: that selects the
  unrelated V4L2 backend. The separate `libfmjni_bcm_hci` soname avoids collisions.
* SELinux adds only `system_app -> bluetooth:unix_stream_socket connectto`.
  No permissive domain, UART/device grants, shell socket access or vendor HAL
  access is added. The broker also authenticates UID and validates requests.

## Implemented paths and deliberate limits

* Power on/off, exact tune/readback, seek in both directions with bounded wrap,
  bounded station scan and cancellation, mute, and receiver digital volume.
* **EU band only:** 87.5–108 MHz, 100-kHz grid, 50-us de-emphasis. Region selection
  for other bands/de-emphasis values is not implemented.
* Uses existing FMRadio favorites/station UI. Requires a wired headset/headphone
  antenna; output can be selected between wired headphones and the speaker.
* Explicit failure messages when Bluetooth is off or the backend is unavailable.
  Link health is checked while playing. Bluetooth shutdown, headset removal,
  focus loss and SCO audio activation stop the FM route; long scans are cancelled
  rather than leaving power-off queued behind a scan.
* **RDS/AF and recording are disabled.** The backend reports RDS unsupported and
  the recording actions are hidden/rejected. No fake station metadata is returned.
* **No FM-to-A2DP/SCO playback**, no secondary-user support, and no automatic
  Bluetooth power management. Normal Bluetooth/Wi-Fi coexistence is a test item,
  not yet a verified claim.

## What is inferred rather than proven

The dump proves the Broadcom stack and SEC audio paths exist. It does **not**
prove that this initialization sequence works with the exact running firmware.

The receiver engine independently implements the register framing and constants
published in Broadcom's V4L2 FM sources (for reference,
[`fmdrv_main.h`](https://github.com/arttttt/SmokeR24.1-kernel/blob/SmokeR24.1-stable/drivers/bluetooth/broadcom/v4l2_fm_driver/fmdrv_main.h)
and adjacent receive-driver files). The code is not an import of that kernel
transport, nor does that other board's existence validate the A6. It expects
Command Complete to include the register/access echo; it fails closed on a
status error, different framing, wrong tune readback or missing completion.

The candidate uses stock RSSI/SNR thresholds 105/10, FM I2S output, muted/zero
volume startup, and the board's existing Bluetooth I2S configuration. It polls
status flags with interrupts masked, rather than installing another event owner.
Whether those flags latch as expected on BCM4345C5 must be checked on the phone.

Audio uses the stock HAL's `l_fmradio_mode=ready/on/off` path plus a looping silent
MUSIC AudioTrack to keep the primary route active. The real audio is the hardware
FM input, **not** a software loopback. Output is selected per AudioTrack rather
than globally forcing other apps to speaker. Receiver gain follows the MUSIC
dB curve because scaling the silent samples cannot control direct FM audio.
The SEC HAL's handling of that route, I2S clocks, volume mapping, speaker output,
and standby still need device validation. A successful AudioTrack call is not
proof that FM sound reached the codec. Test initially at a low volume.

## Build and first test (no stock log needed)

After applying all project patches, use your normal source setup and lunch:

```sh
source build/envsetup.sh
lunch lineage_a6lte-userdebug
m FMRadio libfmjni_bcm_hci libbluetooth
# Then build a complete ROM so the device policy and firmware config are included:
m bacon
```

No complete Android build was run in the dump workspace. Building just these
modules is only an early check; do not sideload an APK alone and expect the
Bluetooth endpoint or policy to be present. Follow the device's normal backup,
installation and recovery procedures; there is no new bootloader/partition step.

1. Keep SELinux enforcing. Verify normal Bluetooth operation and firmware loading.
2. Enable Bluetooth, connect wired headphones and start FMRadio in the primary
   user. Tune a known local station at low MUSIC volume. The first run uses the
   selected frequency instead of automatically seeking, to simplify bring-up.
3. If it fails, use the new ROM's logs (not a stock log):

   ```sh
   adb logcat -c
   # Reproduce once, then:
   adb logcat -d -v threadtime -s BcmFm bt_bcm_fm BcmFmAudio AndroidRuntime audio_hw_primary
   adb shell dumpsys media.audio_flinger
   adb shell dumpsys media.audio_policy
   ```

   On userdebug builds, inspect AVC denials with your usual privileged logging
   workflow. Do not switch to permissive to hide them. Inspect/redact logs before
   sharing. The offline decoder in `tools/fm` remains available for an optional
   HCI capture, but capturing stock is no longer a gate for testing this code.
4. A non-zero controller status or incompatible completion identifies the
   protocol stage to investigate. Successful tune/readback but silence directs
   investigation to the HAL route/PCM/I2S path. After a transport timeout, toggle
   Bluetooth before retrying. Never open the UART from a second process.
5. Test repeated on/off, scan cancellation, mute and volume zero, both outputs,
   headset removal, suspend/resume, app force-stop/crash, calls/SCO, Wi-Fi and
   Bluetooth toggles. Verify radio and FM PCM clocks are off after app exit.

## Host checks

From the device tree, on Linux:

```sh
python3 -m unittest discover -s tools/fm/tests -v
python3 fm/tests/run_host_tests.py --bt-tree "$ANDROID_BUILD_TOP/system/bt"
python3 fm/tests/run_host_tests.py --bt-tree "$ANDROID_BUILD_TOP/system/bt" --sanitize
```

The native suite has 24 mock-controller test groups, including failure injection
at every power-on I/O step. Six broker/socket test scenarios exercise exclusivity,
peer rejection, bad requests, client death, timeouts, late callbacks, adapter
restart/shutdown and callback ownership. The broker tests compile the real broker
against **host mocks** for HCI/audio/peer credentials; they are not Android HAL or
SELinux tests. ASan/UBSan passed in the development workspace.

The patched app Java sources were type-checked against Android 11 API classes
with generated **test** R constants. JNI declarations were checked against
`javac -h` headers, and the JNI source was host-compiled with a log stub. These
checks do not replace Android resource packaging, ABI builds, policy compilation
or hardware tests. All git-am patches are additionally applied to pristine
source snapshots and compared with the authoring trees.
