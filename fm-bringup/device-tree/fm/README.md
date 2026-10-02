# A6 Broadcom FM native backend

See `../docs/fm-integration.md` for the full design, source assumptions, build
steps and hardware caveats. This is an experimental EU-band receiver candidate.

* `BcmFmProtocol.h`: versioned local socket name, exact operation allowlist and
  controller-response validation. Must match the copy in `system/bt/btif/include`.
* `BcmFmRadio.*`: serial receiver state machine, tune/readback, seek, scan, cancel,
  mute and volume. Host-buildable, with no socket dependency; errors go to
  Android logcat on the device and stderr in host tests.
* `FmJni.cpp`: authenticated local socket client and FMRadio JNI API. Uses a
  separate soname; it does not load or copy Samsung's private FM libraries.
* `tests/`: pure C++ core tests and broker tests with mock Android boundaries.
* `RdsDecoder.*` / `RdsCharset.h`: PS, RadioText 2A/2B, PI confirmation, AF lists
  and G0-to-UTF-8 conversion. The charset table retains its upstream license.

RDS JNI methods now use the real decoder; AF requires stronger signal and a
matching confirmed PI. Recording is implemented on the app/audio-policy side
with explicit FM-input checks. See `../docs/fm-rds-recording.md` for limits and
hardware validation requirements.
The stock kernel's V4L2 radio support remains disabled. No UART/device permissions
or Broadcom pin-mux writes are added.
