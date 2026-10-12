# COM8: communication and slide-travel acceptance

Historical R14 record: deployment and integration statements below describe
that checkpoint. The subsequent local transport integration is documented in
motor-travel-interlock-r18.md; it is not a new device acceptance or deployment.

## Current boundaries

- Target: COM8, 192.168.5.18, MAC E8:3D:C1:F7:65:44. Do not scan other
  addresses or use other serial ports.
- R13 remains the last verified deployment. This work has NOT deployed a new
  firmware/filesystem, changed device configuration, or sent motion/relay commands.
- The operator reports a nominal 300 mm vertical slide with no upper/lower limit
  switches. Earlier upper-end contact/noise remains part of the fault history.
- ACK, parameter readback, UI tests and firmware write verification are separate
  from physical motion, stopping, position and long-duration acceptance.

## Communication acceptance

Keep R13's failed 180-second window as FAIL. Its single connect timeout is not
diagnostic of Wi-Fi versus TCP admission versus firmware scheduling on its own.
Do not tune HTTP/AsyncTCP limits or increase timeouts just to make a test pass.

R14 uses serial requests with new-connection, keepalive and Undici transports.
It records connect, headers and body phases where the transport exposes them.
The unauthenticated check and a fresh Reachable matching MAC precede any login.
A precheck failure or zero samples is not a successful observation window.

If a failure repeats, correlate its actual time with passive COM8/device logs and
local link evidence. A matching ARP entry/ping confirms only link identity, not
HTTP recovery. Browser load is uncontrolled unless explicitly measured. Keep
all failed reports and their gaps; do not silently replace them with a retry.

After a demonstrated defect is corrected, use a fresh immutable deployment
baseline, then finite read-only acceptance windows. Continuous testing has NOT
run: continuous24hStatus=NOT_RUN. No unattended motion endurance testing is
permitted on this uncalibrated slide.

## Field calibration still required

Current operator evidence identifies a 1605 screw with 5mm/revolution lead and
DM556 SW5 OFF / SW6-SW8 ON, whose nameplate row is 400 pulses/revolution.
Direct coupling gives a theoretical 80 driver-input pulses/mm. The operator now
reports the slide is in the middle, but this is not a measured 150mm coordinate,
an established origin, a verified clearance or a stationary execution consent.
See image-reference-002.json and operator-observation-001.json in the R14 artifact
directory. Do not synthesize measured samples from these observations.

The frozen R13 service reads/writes raw U16 speed and pulse registers without a
millimeter conversion. The vendor PDF physical pages 3 and 8 describe pulse
resolution and register 7; PDF physical page 11 and XLSX Sheet1 B15/B22 provide
the write/read examples. None of those examples resolves the inconsistent
maximum counts in the supplied product listing. Confirm actual command units
against physical output; driver-input pulses and Web/register values must not
silently be treated as interchangeable. The offline samples must pair the actual
command count with independent measured displacement on the same unchanged setup.

Do not enter 300 into the existing pulse-domain limit fields. Establish:

1. Verified driver microstep setting and mechanism transmission, or at least two
   independently measured pulse/distance pairs from supervised finite moves in
   an already known-clear interior region. These measurements have not been made
   by this work. Do not command a move until its displacement is safely bounded.
2. A repeatable physical origin, positive direction and actual usable travel.
   Do not find either endpoint by collision, stall, abnormal noise or an automatic
   sweep. A model label is not a measured endpoint.
3. End margins accounting for position uncertainty, backlash and stopping
   distance under the actual load. No arbitrary default margin is supplied.
4. A separate physical end-limit/stop and lift load-retention assessment. Software
   coordinates cannot guarantee containment after lost steps, manual movement,
   restart, controller reset, drift or mechanical/load failures. Do not assume
   cutting power safely holds a vertical load.

Fill a separate copy of the measurement template only with actual measurements.
The offline calculator has no network or serial I/O and never enables limits:

    node scripts/plan-motor-travel.mjs --measurements=<measured-input.json> --out=<unused-review.json>

Its candidate pulse bounds deliberately remain deployable=false and
referenceKnown=false. Repeated measurements and their tolerance still need
engineering review; observed spread is not a guarantee of future position error.

## Software candidate and remaining integration gate

MotorTravelAdmission.h is an OFFLINE CANDIDATE, not currently called by the
production transport or MotorControlService. Its tests establish the component's
behavior only, not that a real API/RAW/MQTT request is currently blocked.

The candidate provides per-axis volatile reference and possible-position bounds,
fresh finite-pulse readback, zero-write rejection at the adapter test boundary,
stationary confirmation independent of ACK, and a separate configured stop path.
Reservations occur before a start; lost replies invalidate the reference instead
of reclaiming travel. Reversing does not cancel earlier position uncertainty.
Explicit stationary confirmation preserves, rather than narrows, the interval.
It supports arbitrary slave IDs and distinct FC06/FC16 action values, while an
unrelated relay address is outside that axis's policy.

Before any production release, integrate and verify ALL of the following:

- Admission under the existing Modbus transaction semaphore, before UART output,
  covering structured calls, execution rules, MQTT, raw and broadcast paths.
- Keep per-axis state for the lifetime of the handler. Reset on configuration,
  bus reinitialization/reboot or ambiguous outcomes. Never hydrate a reference
  from persisted motorCurrentPosition or a successful command echo.
- Remove the old service/route estimate-based limit decisions. They cannot be
  layered over this candidate because their parameter prewrites would invalidate
  fresh readback and their ACK-based position arithmetic overstates certainty.
- Authenticated, configuration-bound manual reference/stationary confirmation,
  serialized against queued/running controls. The Web and physical-panel views
  must display unknown position and cannot silently confirm it on the user's behalf.
- End-to-end production harness tests proving rejected commands reach zero UART
  writes; tests of stop priority, raw/broadcast bypass, configuration races, reboot,
  stale reference/readback, finite range overflow and independent relay operation.
- Freeze an isolated candidate based on the deployed baseline, run build/Web
  gates, preserve configuration, verify actual write/readback, then obtain supervised
  physical acceptance. Never flash the unrelated dirty root worktree wholesale.

The standalone candidate is intentionally not auto-enabled while these gates
and field measurements remain open.
