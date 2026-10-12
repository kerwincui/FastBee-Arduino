# Motor travel interlock: local R18 candidate

## Status and scope

This is a LOCAL SOFTWARE CANDIDATE, not an enabled or deployed slide limit.
It integrates the offline R14 admission component into the actual Modbus
transport. Historical R14 and device acceptance reports remain unchanged.

This work makes no device requests, opens no COM port, changes no device
configuration, and issues no motion/relay command. The last saved R17 read-only
evidence still has limits disabled and no known reference; it was not refreshed
here. The frozen build comes from the current workspace, not a deployment-approved
minimal overlay on R17. Do not flash this entire workspace over the device.
Other work in the shared checkout is deliberately preserved.

## Implemented protection

- A per-bus, per-axis interlock runs before UART transmission, under the existing
  transaction semaphore. Structured, raw, rules and MQTT paths reaching that
  transport use the same gate. Disabling polling does not remove the guard.
- Motor mappings come from configuration: slave, registers, FC03/FC04 reads,
  FC06/FC16 writes, action values and finite-pulse semantics. No slave8 or YF53
  special case is used. Unrelated relay addresses keep their existing behavior.
- Starts require a usable finite-move mapping, configured bounds, an explicitly
  confirmed reference and stationary condition, fresh pulse readback, and room
  for the whole requested move. There is no silent pulse clipping.
- Travel is reserved before TX as a conservative possible-position interval.
  An ACK is never treated as measured travel. Reversal and stationary
  confirmation do not cancel or narrow accumulated uncertainty.
- Restart, configuration/bus reset and ambiguous/failed protected transactions
  lose reference trust. No persisted coordinate restores it automatically.
- An unambiguous configured STOP remains available without a reference. Protected
  writes are not automatically retried. A STOP invalidates position trust;
  its acknowledgement does not prove that the physical load stopped.
- Broadcasts and unknown/multi-register protected writes cannot bypass the gate.

## Confirmation workflow

The protocol motor panel contains a separate travel-confirmation section. Its
position field accepts integer CONTROLLER COUNTS, not millimeters. The old
persisted position field is read-only historical data, not a homing mechanism.

1. An authenticated cached state read supplies the current motor binding and
   boot/generation token. It does not itself read or move the physical motor.
2. The operator must explicitly assert that the slide is stationary and its
   load is safe. Neither the UI nor an ACK supplies this assertion.
3. confirmReference supplies the independently established physical coordinate
   in controller counts. confirmStationary only asserts that motion ended;
   it cannot create a lost reference or narrow the possible-position interval.
4. Stale tokens, changed bindings, invalid counts and queued/running commands
   reject confirmation. Confirmation does not enqueue a movement or send UART.
5. Confirmation consumes the UI state and invalidates pulse-readback freshness.
   Obtain a new parameter readback before attempting a start.

The screen distinguishes disabled limits, missing reference, unconfirmed motion
completion and unavailable travel. Each direction uses its own admission flag;
STOP remains separate. The API keeps motionKnown=false and never presents
the conservative interval as actual position feedback.

## Physical activation remains blocked

Do not remove the warning or enter nominal 300 mm as pulse-domain bounds.
The reported 5 mm screw lead and driver switch setting are not a measured
controller-count/mm conversion. Earlier remaining clearances are not a current
coordinate. No fabricated calibration inputs are supplied by this change.

Required field evidence remains:

1. Verified controller count units and independently measured displacement for
   supervised finite moves within a confirmed clear interior region. Do not
   command a move whose displacement is not safely bounded.
2. A repeatable origin, forward-direction definition and actual usable bounds.
   Do not find an endpoint by collision, stall, noise or automatic sweeping.
3. Reviewed uncertainty, backlash/load/stopping allowances and end margins.
4. A physical end-limit/stop and vertical-load retention assessment. Software
   counting is not a substitute for these protections.

The existing offline calculator can review real measurements without touching
hardware:

    node scripts/plan-motor-travel.mjs --measurements=<measured-input.json> --out=<unused-review.json>

Its output remains a non-deployable proposal. After review, prepare an isolated
release, verify the exact COM8 identity and preserved configuration, deploy and
read back that artifact, then confirm the measured reference. Supervised short
movements must establish actual displacement and self-stopping. A test script
that sends STOP after a timeout does not demonstrate natural self-stopping.

Reboot, manual displacement, a stop or uncertain communications requires a new
reference confirmation before further motion. Long-term stability and physical
limit containment are NOT verified by local tests.

## Reproducible local tests

Evidence is stored under artifacts/motor-travel-interlock-r18/. Use a new run
number/output directory each time. Never overwrite a failed report.

    & 'C:/Users/kerwin/.platformio/penv/Scripts/python.exe' artifacts/motor-travel-interlock-r18/verify.py 003
    node scripts/test-motor-travel-browser.cjs artifacts/motor-travel-interlock-r18/browser-003
    & artifacts/motor-travel-interlock-r18/build.ps1 -Run 002

- local-tests-001: FAIL, two test-runner import errors; retained. The runner's
  module path was fixed. This was not a device failure.
- local-tests-002: PASS, 57 tests, zero failures/errors/skips; source stable.
  Includes real production UART-path harnesses, configuration/queue/API cases,
  generic protocol and relay regressions, screen raster and 63 screen blocking
  combinations with STOP available.
- browser-001: FAIL at 390px because the fixed-width debug column squeezed the
  control panel. Retained; the scoped mobile layout was corrected.
- browser-002: PASS at 1280, 390 and 320px. Real fragment/style/module with
  mocked API; explicit assertion, integer parsing and replay checks; zero network
  requests/motion commands, no horizontal clipping, and saved screenshots.
- Build-only evidence is recorded separately by the RGB builder under
  artifacts/rgb-resume-motor-travel-interlock-r18-001-build.json.
  PASS: exitCode=0, sourceStable=true; binary size 3,676,080 bytes, SHA256
  d9639bdbf15c5dbfa1922cbf22adfe607d2357475e2f5223bd784ce54641e34f.
  The firmware hash and frozen source digest were independently rechecked.

Subsequent shared-workspace edits to BoardProfile.h/.cpp, DriverRegistry.h and
PeripheralRouteHandler.cpp are not covered by this frozen build. The motor
interlock files and browser sources still match the tested candidate. This
is not a build claim for the entire latest working tree. The shared build lock
was released normally; no build/test process from this work remains running.

Deployment, physical calibration, activation, motion acceptance and long-duration
acceptance remain NOT_RUN. continuous24hStatus=NOT_RUN.
