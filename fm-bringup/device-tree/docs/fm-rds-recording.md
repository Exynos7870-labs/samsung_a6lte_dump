# RDS, AF and FM recording follow-up

This extends the initial receiver candidate to implement the metadata and recording
paths used by LineageOS FMRadio `lineage-18.1`. It is **source implementation, not a
claim of working A6 hardware**. Android/SELinux builds, the exact firmware FIFO,
physical audio routing and encoded recordings still require device validation.
No stock log is required before building this candidate.

## RDS transport and decoding

The authenticated socket is now `a6lte.bcm_fm.v2`. Upgrade both the Bluetooth and
JNI sides together. The previous v1 endpoint cannot accept RDS operations.

The additional allowlisted FC15 operations are:

* SYS `00 = 03`: FM plus RDS (00/01 remain off/FM-only).
* RDS control `02 = 02`: RDS mode, flush FIFO.
* Waterline `14 = 64`: tuples, not bytes.
* FIFO `80`, read length **240 only**. Replies must retain the matching register,
  access echo, successful status and exact HCI event length. Only this register
  accepts shorter payloads: 0..240 bytes, divisible by three. Other registers
  retain strict scalar-width checks. There is no arbitrary memory/pin access.

The reference describes triples `[status, MSB, LSB]`, block types A/B/C/D/C-prime,
quality bits 3:2 and the empty/end tuple `7c ff ff`. The implementation polls the
FIFO every 250 ms while playing, rather than adding an asynchronous HCI event
owner. Masks remain zero. **Polling without an interrupt and exact FIFO reply
framing have not been observed on an A6**. Malformed replies fail closed.

The independent decoder handles:

* Ordered groups, C-prime PI validation, corrected versus uncorrectable blocks,
  partial groups across reads, empty markers and bounded malformed data.
* PI confirmation from two groups before accepting station metadata.
* PS from groups 0A/0B, with repeated segments and protection against mixed names.
* RadioText 2A (64 characters) and 2B (32), repeated segments, terminators,
  A/B and version changes, and incomplete-message suppression.
* RDS G0 character conversion to UTF-8, including accented European characters.
  Control characters are not forwarded into UI/database fields.
* Station changes and 15-second data loss invalidate metadata. Tune, search,
  disable and power-down reset caches; tuner changes flush the hardware FIFO.
* Complete repeated Method-A AF lists and repeated Method-B pairs containing
  the tuned transmitter. Filler, LF/MF escapes and other-network EON lists are
  not mistaken for FM alternatives.

The real `setRds`, `readRds`, `getPs`, `getLrText`, `isRdsSupport` and `activeAf`
JNI functions replace the previous stubs. Event bits match the app: PS 0008,
RT 0040 and AF 0080. Java explicitly decodes UTF-8. The BCM service polls on its
existing tuner handler, not the legacy RDS thread, so metadata cannot race a
handler-owned tune/seek/scan. It updates listeners, the station database and
notifications. RDS resumes after failed/restored tunes and seeks too.

This implements the app's PS/RadioText/AF contract, not every optional RDS
application: CT display, EON traffic switching, RT+, ODA and extended UCS-2 text
are not added to the app.

## Alternative-frequency policy

A complete AF list alone never requests a blind retune. The receiver first needs
weak signal (RSSI magnitude at least 105, using the reference's conversion).
Probes mute the receiver, try at most two candidates per attempt, and require
at least a 6 dB improvement plus a twice-confirmed **identical PI**. Each PI wait
is limited to 1.2 seconds; probing has a five-second budget checked between I/O,
in addition to per-transaction timeouts. Restoration can take additional time.

Every probe restores the original frequency and mute state before returning a
recommendation. A cancelled probe stays muted. There is a 30-second retry
cooldown. Java applies the recommended tune through the existing UI workflow
only if no stop, focus loss, user tune/seek/scan, power-up or recording request
has superseded it. AF probing is suppressed during recording. Thresholds and
mute/retune behavior need field validation; there is no seamless-handover claim.

## Recording

The Record/Recordings menu actions are enabled again. `BcmFmCapture` opens
`AudioSource.RADIO_TUNER` at 48 kHz, stereo PCM16, explicitly selects an
`AudioDeviceInfo.TYPE_FM_TUNER` input and verifies the actual routed device's
**type and ID**, not just `setPreferredDevice()` success.

* Discard startup data until the route is established; time out after one second.
* Reject microphone/call/mix fallback, unavailable input, wrong device, read
  errors, non-frame-aligned data, route changes and a two-second input stall.
* Recheck the actual route before and after every read. Feed only verified FM
  PCM to the existing FMRadio AAC/M4A encoder and save/discard/media-database
  workflow. Do not render these samples on top of direct hardware playback.
* Stop capture immediately on Bluetooth/headset/focus/service shutdown;
  finalize recording through the existing recorder workflow. Stop even on duck
  focus loss rather than silently recording an unintended source.
* Use a nonblocking read loop and a single release owner. Synchronize stop with
  sample delivery so an old capture cannot feed a later recording session.

The A6 policy copy comes from pinned common-device revision
`a433c37b08659956b366e06019c36e310a0b2781`,
`configs/audio/audio_policy_configuration.xml`. Its existing FM Mic port and
primary-in route are unchanged; **only FM Mic is added to attachedDevices**.
The A6 product copy precedes common inheritance so it wins the duplicate copy
file destination. Verify the installed policy after building. No common/vendor
repository patch or direct app access to ALSA devices is required by this design.
The app explicitly requests signature-protected CAPTURE_AUDIO_OUTPUT for the
RADIO_TUNER source, in addition to its existing recording permission.

The AAC helper also fixes the upstream asynchronous muxer startup: it waits for
`onOutputFormatChanged` and codec-specific data, skips codec-config packets,
tracks all available input buffers, drains one EOS, bounds queued PCM to 1 MiB,
limits EOS waiting to three seconds and releases resources/error waiters on
failure. Early errors are retained until a listener is registered. These fixes
also benefit the legacy capture path because it shares the encoder.

The policy declaring an FM input does not prove the SEC HAL actually captures
FM samples. Actual-route checks protect against a framework-level fallback;
they cannot prove the physical HAL source. Check real recorded audio and input
routing before calling recording functional. HAL gain/mute interaction with
recording is also unverified.

## Sources and licensing

Protocol facts were inspected in Broadcom's reference driver, not imported as a
kernel transport:

* [fmdrv.h](https://github.com/arttttt/SmokeR24.1-kernel/blob/9582e3a65caff954e40c8795bd2a909b2e4d92b8/drivers/bluetooth/broadcom/v4l2_fm_driver/fmdrv.h)
* [fmdrv_main.c](https://github.com/arttttt/SmokeR24.1-kernel/blob/9582e3a65caff954e40c8795bd2a909b2e4d92b8/drivers/bluetooth/broadcom/v4l2_fm_driver/fmdrv_main.c)
* [fmdrv_rx.c](https://github.com/arttttt/SmokeR24.1-kernel/blob/9582e3a65caff954e40c8795bd2a909b2e4d92b8/drivers/bluetooth/broadcom/v4l2_fm_driver/fmdrv_rx.c)

`RdsCharset.h` adapts only the EN 50067 G0 table from
[redsea rdsstring.cc](https://github.com/windytan/redsea/blob/7555c9f6259d50718697ee8c9f218ea012c6892c/src/text/rdsstring.cc).
Its permissive copyright/license notice is retained in that file. The decoder
and receiver integration are independently implemented under Apache-2.0.

## Tests and hardware checklist

Host suites: 32 receiver/control groups (including RDS initialization failures,
FIFO errors, AF identity/hysteresis/restoration/cancellation), 16 decoder groups
(including 20,000 deterministic malformed inputs), 7 actual-broker/socket
scenarios, 8 Java capture scenarios and 6 encoder lifecycle scenarios against
mock Android audio/codec/muxer boundaries.
The 22 offline HCI decoder tests remain. Native/RDS/broker tests pass ASan/UBSan;
Java sources type-check with Android 11 classes and synthetic test R constants.
These are not Android resource, policy, codec or hardware tests.

From the respective patched projects:

```sh
# device/samsung/a6lte
python3 fm/tests/run_host_tests.py --bt-tree "$ANDROID_BUILD_TOP/system/bt" --sanitize
# packages/apps/FMRadio
python3 tests/run_capture_tests.py --java-home /path/to/a/host/jdk
```

On the phone, additionally verify:

1. PS and 2A/2B RadioText on known broadcasts; accents, changing text, weak signal,
   retunes, no RDS on a station, repeated on/off and Bluetooth toggles.
2. No stale metadata saved against a new station. No wakeups after service exit.
3. AF must retain the same programme; cancellation/calls must restore/stop audio.
   Verify no probes or programme changes occur during recording.
4. The installed policy exposes FM Mic. Verify actual FM input routing using
   `dumpsys media.audio_flinger` and `dumpsys media.audio_policy`.
5. Play back saved M4A files; check content, channel count, duration, save/discard,
   storage-full/unmount, routing changes, calls, headset removal and app death.
   An error or silence must not be reported as proof of working FM capture.

Useful log tags now also include `BcmFmCapture`, `FmService`, `FmRecorder` and
`AudioRecorder`, alongside `BcmFm`, `bt_bcm_fm`, `BcmFmAudio` and AndroidRuntime.
Review/redact logs before sharing.
