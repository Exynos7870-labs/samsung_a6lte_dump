# A6 FM radio — experimental LineageOS 18.1 integration

A **source-based receiver integration candidate** is now implemented using the
current vendor tree, stock dump and available source. A stock log is **not a
prerequisite**. This is more than the earlier firmware/diagnostic preparation,
but **reception and audio have not been verified on an A6** and a complete
Android/SELinux build has not been run here.

The current vendor is
[`samsungexynos7870/android_vendor_samsung_a6lte:lineage-18.1`](https://github.com/samsungexynos7870/android_vendor_samsung_a6lte/tree/lineage-18.1),
commit `f0c55d795a9be60147d76a42ec93fd9ba3e5800f`. Its
`bcm4345C5_V0069.0172.hcd` is **byte-identical** to this dump's firmware (SHA-256
`fe5d61771d2e26310f295ee7241b5d188b80d80d3b6805f4a6452d52783a2225`). No new blobs,
stock APK transplants, kernel radio drivers or UART permission changes are needed
by this design.

## Implemented

* **Bluetooth-owned transport:** a device-gated, authenticated local endpoint
  submits a small allowlist of FM register commands through Fluoride's existing
  HCI queue. It handles client death, controller responses, timeout and adapter
  shutdown/restart. It does not open the UART independently or expose raw HCI.
* **FMRadio native backend:** EU-band power, tune/readback, seek/wrap, scan/cancel,
  mute and digital volume, with bounded waits and failure rollback.
* **App/audio integration:** separate JNI soname, explicit Bluetooth requirement,
  wired antenna, headset/speaker output, MUSIC-volume tracking, link monitoring,
  focus/SCO/headset lifecycle and an experimental Samsung SEC-HAL direct route.
* **Device wiring:** package/overlay/dependency selection, correct firmware name,
  and one narrow SELinux socket-connect rule. No permissive policy.

**Current limits:** Bluetooth must already be enabled; primary Android user only;
87.5–108 MHz / 100 kHz / 50-us EU settings. **RDS/AF, recording and FM-to-Bluetooth
playback are disabled.** The exact firmware completion behavior, SEC HAL audio
sequence, gain mapping, clocks and coexistence require phone testing. Failures
should be diagnosed with the new ROM's logs; a stock log can be added later.

Read [the integration notes](device-tree/docs/fm-integration.md) for design,
source assumptions, build steps and the hardware test checklist.

## Patch series

All **five patches** were applied using `git am --whitespace=error` to pristine
source snapshots, and each resulting tree was compared with its authoring tree.

| Target checkout | Patches |
| --- | --- |
| `device/samsung/a6lte` | [0001](patches/0001-a6lte-select-the-shipped-Broadcom-firmware.patch), [0002](patches/0002-a6lte-document-FM-transport-and-add-stock-trace-decoder.patch), [0003](patches/0003-a6lte-integrate-experimental-Broadcom-HCI-FM.patch) |
| `system/bt` | [Bluetooth companion](patches/system_bt/0001-bt-add-device-gated-Broadcom-FM-endpoint.patch) |
| `packages/apps/FMRadio` | [FMRadio companion](patches/FMRadio/0001-FMRadio-add-Broadcom-HCI-and-SEC-audio-support.patch) |

The first two device patches are preserved from the previous preparation step.
**Patch 0003 contains the device-side implementation**, while both companion
projects are mandatory. Do not apply another project's patch in the device tree.

Device base:
[`android_device_samsung_a6lte:lineage-18.1`](https://github.com/samsungexynos7870/android_device_samsung_a6lte/tree/lineage-18.1)
`a6b97426994a2d03df18087ed8b8c4c84a69905c`.
Bluetooth base: LineageOS `lineage-18.1`
`bcca9a572f1cc539ba21ebe940c3642ce8707d1a`.
FMRadio base: LineageOS `lineage-18.1`
`c8664126026de37a442dba996db8bc4d3606dc41`.

Use the supplied [LTS manifest layout](https://github.com/samsungexynos7870/android_manifest_samsung_j5y17lte/blob/lineage-18.1-lts/j5y17lte.xml),
substituting the A6 device/vendor projects. The common-device revision is
`lineage-18.1-oss_bsp-vndk`, **not** plain `lineage-18.1`; kernel and common vendor
use `lineage-18.1-lts`. Exact inspection revisions are in `source-revisions.json`.
If FMRadio is missing, sync `LineageOS/android_packages_apps_FMRadio` at
`lineage-18.1` into `packages/apps/FMRadio` first. Apply patches after the final
source sync. Patches for the target ROM projects are provided here; they have
not been applied to those upstream repositories.

### Apply with git am

From the **ROM source root**, after checking each checkout is clean and has no
in-progress am operation:

```sh
P=/absolute/path/to/samsung_a6lte_dump/fm-bringup/patches

git -C system/bt am "$P"/system_bt/0001-*.patch
git -C packages/apps/FMRadio am "$P"/FMRadio/0001-*.patch
git -C device/samsung/a6lte am "$P"/000*.patch
```

**If you already applied device patches 0001 and 0002**, apply both companion
patches as above, but use only this device command:

```sh
git -C device/samsung/a6lte am "$P"/0003-*.patch
```

If a patch conflicts, stop and inspect it; `git -C <affected-project> am --abort`
aborts that project's in-progress operation. Applying multiple projects is not
atomic: already-applied commits in other projects are not automatically undone.
Do not force-apply to unrelated revisions.

## Build and try

```sh
source build/envsetup.sh
lunch lineage_a6lte-userdebug
m FMRadio libfmjni_bcm_hci libbluetooth
m bacon
```

Build a complete ROM to include the policy/config changes. An APK-only install
is insufficient. Use the device's normal backup/installation/recovery procedure.
Enable Bluetooth, insert wired headphones, select a known station and start at
low volume. Keep SELinux enforcing. See the integration notes for failure logs
and lifecycle/coexistence tests. None of these phone tests has been performed in
the dump-only workspace.

## Checks completed

* **22** offline btsnoop-decoder tests.
* **24** native mock-controller test groups, including failure injection at every
  initialization I/O step, response validation, exact tune readback, seek/scan,
  cancellation, timeout, volume limits and power-down cleanup.
* **6** broker/socket scenarios: authentication, single-client ownership, invalid
  operations, client death, late callbacks, timeout and adapter lifecycle.
* Native core/broker suites passed **ASan and UBSan**. Android HCI, audio and peer
  credentials are mocked in broker tests; the actual broker/socket code is used.
* Patched app Java sources type-checked against Android 11 API classes with
  generated **test R constants**. This is not resource/APK packaging.
* All **18 JNI exports** matched Java declarations; JNI source host-compiled
  against `javac -h` declarations with an Android log stub.
* All five patches passed the three-project git-am round trip.

**Not verified:** full Android/ABI builds, resource packaging, SELinux compilation,
firmware loading under the new ROM, reception, sound, power consumption, or
Bluetooth/Wi-Fi/call coexistence on a real phone.

## Files and reproduction

* `device-tree/`, `bluetooth-stack/`, `fm-app/`: readable overlays containing only
  changed/new files, **not complete source trees**. Do not replace whole projects
  with these directories; apply the patches instead.
* `patches/`: project-separated mail patches and series files.
* `stock-evidence.json`: stock configuration, file hashes and current-vendor
  firmware comparison. `source-revisions.json`: inspected Git revisions.
* `device-tree/tools/fm/`: optional offline stock/new-ROM HCI trace filter. Review
  filtered output before sharing; don't upload raw Bluetooth logs or bugreports.
* `make-patches.py`: regenerate the implementation patches and test them using
  pristine source snapshots. Preserves the original device patches 0001/0002;
  never commits or resets the inputs or this dump repository.

From this directory:

```sh
python3 -m unittest discover -s device-tree/tools/fm/tests -v
python3 device-tree/fm/tests/run_host_tests.py --bt-tree bluetooth-stack --sanitize
python3 make-patches.py /path/to/pristine-device \
  --bt-tree /path/to/pristine-system-bt --fm-tree /path/to/pristine-FMRadio --sanitize
```

Keep captures, firmware copies, downloaded build tools and generated binaries
outside Git. The host tests are not intended to run on a phone.
