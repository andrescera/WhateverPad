# Todo 2: B5/B6 repair

Date: 2026-10-23. Worktree:
`/home/andres/ps5/build/whateverpad-wt/foundation-v2-t2`.
Branch: `wave0/t2-reproducers`.
Starting candidate: `3cfc702a415645797c408dd0377a26c1fe2f7190`.
Owner window: `2026-10-23T05:23:06Z` to `2026-10-23T09:23:06Z`.
The first command, `date -u`, returned `2026-10-23T05:24:11Z`, exit 0.

This report supersedes the B5/B6 deferrals in the previous task-2 report, not
its historical review receipts. The three rejected reviews remain rejected;
this repair does not claim independent acceptance, main integration or hardware
verification.

## Canonical baseline integration

`git merge --no-ff github/main` completed without conflicts, exit 0, producing
merge commit `7d2b50e46cc782893727465b57c12641cfb13030`.
Its parents are the starting candidate and canonical main
`b8629ed17460a8309868437921a95a8de05a3e4f`.
Main's build, CI, tools and documentation changes were retained. The inherited
test changes were retained. The already-shipped web-fixture repair appears once:
`host_close(g_host)` runs before `web_stop(g_web)` and `sim_free(sim)`.

## Permanent independent oracles

These tests use the actual production host, vpad and HTTP implementations.
The oracles count delivered HCI events, actual submitted Disconnects, callback
effects, vendor calls and complete HTTP response bytes; they do not consult
production ownership, session, generation or vpad-slot fields.

| Ledger entry | Stimulus source | External oracle | Baseline status / first strict error |
|---|---|---|---|
| `reused-handle-without-terminal-never-disconnects` | Review r3 Appendix A / B5; T `test_retained_completion_and_handle_quarantine`, handle half | Real Classic input, public silent vanish and reconnect; two successful Connection Complete events reuse handle `0041`, with no intervening Disconnection Complete. Closing must submit no Disconnect. | XFAIL, todo 14: `wire_disconnects == 0 (todo-14)` |
| `web-stop-before-queued-creation` | RV1 Appendix C; review r3 Appendix B / B6; T `test_stop_before_queued_creation` | A real complete POST Stop precedes the same `vpad_poll` ordering as `ps5_main`. No queued Add, Bind, Delete or input is allowed. | XFAIL, todo 10: `adds == 0 && binds == 0 && deletes == 0 && frames == 0 (todo-10)` |
| `web-stop-drains-identification-without-queued-creation` | Review r3 Appendix B / B6; T `test_release_drains_identification_without_creating_queued_pad` | One actual Add is pending and a second slot is queued. Real POST Stop precedes log identification and its virtual ambiguity boundary. The identified Add must be deleted once, without Bind, second Add or input. | XFAIL, todo 10: `adds == 1 && binds == 0 && deletes == 1 && frames == 0 (todo-10)` |

The B5 hard control pairs through usable input without a lost terminal and
observes one ordinary Disconnect and its callback. The negative case separately
asserts that the reused wire handle was actually delivered and input resumed.
Both close and uninitialize the simulator exactly once.

The B6 hard control sends a complete real State request instead of Stop, then
requires Add, successful identification, Bind and one submitted input frame.
The kernel-log pipe emits an identification event only after the real production
Add reaches its vendor stand-in. Cleanup removes every created device, repeated
removal cannot duplicate Delete, credentials are restored, and every socket and
pipe is closed.

Only the vpad object's socket/connect calls use the pipe-backed log stand-ins.
HTTP uses actual TCP sockets. A web-object-only bind observer forwards the real
OS bind and records the OS-assigned ephemeral port with `getsockname`, avoiding
PID-derived port collisions. Readiness is registered before sending the request;
bounded `poll` waits for response data or EOF. There are no sleeps or retries.

The canonical Makefile still runs the existing `test_vpad` binary. Its recipe
now links the real web/core code and isolates the vpad log and web bind seams
in two objects. The existing `sanitize` target inherits that registration;
`ps5.mk`, CI, tools and production files need no corrective edits.

## Accounting and remaining obligations

The five inherited failing scenarios remain unchanged: three native-release
cases, partial startup, and missing disconnect callback. The sixth inherited
identity, `LE-Create-ownership-through-terminal`, remains two hard-passing
controls with delayed and lost terminal delivery. No sixth failure is invented.

The previous 51 registrations remain in place. This repair adds one session
entry and two vpad entries: **46 session / 2 USB / 3 vpad / 3 web = 54**.

Focused strict commands already exited 1 without hard assertion failures:
`make build/test_session && ./build/test_session --expected-failures`
reported 46 session registrations, and
`make build/test_vpad && ./build/test_vpad --expected-failures`
reported three vpad registrations. The no-loss session control used handle
`0041` and emitted one Disconnect; silent loss delivered the same handle twice
and still emitted the forbidden Disconnect. Vpad mode 0 observed
Add/Bind/Delete/input `1/1/0/1`; queued Stop observed `1/0/0/0`; pending Stop
observed `2/1/0/1`. These are the expected production failures, not runner errors.

Only genuinely unavailable direct-unit portions remain deferred:
`link_bind_handle`/ambiguous session-reason assertions belong to todo 14, and
session-bound quiesce/finalization assertions belong to todo 10. Public silent
drop/reconnect and real HTTP Stop effects are no longer deferred. The production
defects themselves are intentionally not fixed in this test-only lane.

## Canonical verification

Monitor `mon_N8TR8PWGT7EG4NDV` completed with exit 0:

```sh
make clean
env -u MAKEFLAGS -u MFLAGS unshare -Urn -- bash -c \
  'ip link set lo up && make test && make sanitize'
```

`make clean`, `ip link set lo up`, `make test`, and `make sanitize` each exited
0. The namespace isolates the inherited PID-derived web-test ports from other
worktrees; it does not replace any production interface. Both canonical targets
ran every registered binary, including the new coverage in `test_session` and
`test_vpad`. There were no hard failures or sanitizer diagnostics. The session
suite reported 558 checks and USB lifetime reported 659 checks. The unchanged
host and HearBridge suites reported 1244 and 43 checks respectively. The ten
METRIC lines remained identical in normal and sanitizer runs to the historical
baseline below.

Monitor `mon_MKMV8DE8F6EGQVPT` then completed the sequential strict audit runner
with exit 0. Each child process below actually exited **1**, as required:

| Command suffix, run from the worktree | Exit | XFAILs |
|---|---|---|
| `./build/test_session --expected-failures` | 1 | 46 |
| `./build/test_hci_usb --expected-failures` | 1 | 2 |
| `./build/test_vpad --expected-failures` | 1 | 3 |
| `./build/test_web --expected-failures` | 1 | 3 |
| `./build/san/test_session --expected-failures` | 1 | 46 |
| `./build/san/test_hci_usb --expected-failures` | 1 | 2 |
| `./build/san/test_vpad --expected-failures` | 1 | 3 |
| `./build/san/test_web --expected-failures` | 1 | 3 |

Strict children used `ASAN_OPTIONS=detect_leaks=1` and ran sequentially in a
fresh loopback namespace. Their actual pipeline statuses were checked before
continuing. Generated receipts are `build/strict-test_*.log` and
`build/san/strict-test_*.log`. A resident-kernel audit read each complete receipt
with the read tool, matched every name, todo and first error against the 51
historical rows plus the three additions above, and rejected missing,
extra or duplicate entries, XPASS, hard failures and sanitizer errors:
`LEDGER_AUDIT_PASS 54 normal + 54 sanitizer`.
Both session receipts retained the five inherited XFAILs and both hard-passing
LE-Create controls. No assertion was weakened, removed or skipped.

Language-server diagnostics on both changed C files were clean before the
canonical build. Full corrective-diff inspection and `git diff --check`
passed. Production, `ps5.mk`, CI and tools match the merged canonical main.
Only tests, minimal Makefile registration and this evidence file are in the
corrective commit. No console, container, push or merge into main was used.
Fuzz, console behavior and independent gate acceptance were not checked in
this bounded repair.

## Historical report before the B5/B6 repair

# Todo 2: permanent effect-level reproducers

Date: 2026-10-23. Worktree: `/home/andres/ps5/build/whateverpad-wt/foundation-v2-t2`.
Branch: `wave0/t2-reproducers`. Base: M, `2ce2933a1dc8fe7cbdb4bf40efe417d1082a8a03`.
T source: `/tmp/wp-src/task3`, `4b5342859f6ee613afae755eb49611ad84b328a4`.

## Scope and method

Only tests and uncommitted evidence/build-fragment artifacts are changed.
No production source, main branch, console, push, merge, or subagent was used.
The original unchanged clean baseline passed before the first edit: `rm -rf build && make test`,
exit 0 (monitor `mon_5GZ7VAG93PKHR4VF`, terminal `bash_2`).
Baseline counts: 125 session, 428 USB lifetime, 1244 host, 19 lock, 43 HearBridge;
all six original session XFAILs were present.

New assertions observe command submissions, callback counts, simulator-side foreign
procedure completion, real USB stand-in syscalls, vendor Add/Bind/Delete/input calls,
complete HTTP framing and body values, or virtual time. They do not read production
ownership fields. All six inherited scenario identities remain. Attempt 2 replaces
the prohibited sixth oracle with wire Status/terminal observations, zero Cancel,
callback counts, transport close/uninit and the virtual teardown deadline. The
delayed-terminal scenario and a lost-terminal control both hard-pass on unchanged
production. It is not an XFAIL or XPASS registration. The other five inherited
failures retain their assertions and map old task-3/4 labels to v2 todos 15/16.

Strict mode is not a print-only diagnostic: each false expected effect increments
the binary's pending count and causes process exit 1 with `--expected-failures`.
Hard fixture assertions still fail normal mode. M-passing controls are hard assertions,
not registered XPASS cases. No test-only session or ownership implementation is added.

A4/A8 qualify T's positive setting controls: even a successful wire ACK without an
original-value lease and fresh barrier cannot require undo. The ACK-only cases
therefore require zero cleanup too; they are not a claim that M has either v2 policy.
G/H retain the earlier real OFF/mask ACK and refusal bytes. For LE, the actual
`le_poll()` caller submits ON, rather than directly fabricating `le_scanning`.
Both families include noninterleaved controls. M already restores incorrectly in
those controls; all eight G/H cases are expected failures. The post-stimulus raw
read only dispatches available wire events; no private lease field is asserted.

The Appendix A four-order fixture ports T's independent foreign-terminal seam:
after 600 virtual milliseconds the foreign procedure finishes only if a local
Cancel has not already destroyed it. The required Cancel count is always zero.
The late-link close control follows A4: a pending/unproven link grants no Disconnect.

## Registered expected failures

In the table, `first error` is the exact strict-mode line after `FAIL <test>: `,
including its todo suffix. This defines the complete registered ledger; passing
variants are in the next table. Verification receipts below bind it to actual runs.

| Test | Stimulus source | Observable asserted | M status | First error |
|---|---|---|---|---|
| native-idle-full-release | M test_session.c:155-198 | Transport closes on unknown native request | XFAIL, todo 16 | desired observable effect absent (todo-16) |
| native-during-pair-full-release | M test_session.c:155-198 | Transport closes during Pair | XFAIL, todo 16 | desired observable effect absent (todo-16) |
| native-preempts-same-CoD-setup | M test_session.c:155-198 | Transport closes before same-CoD setup proceeds | XFAIL, todo 16 | desired observable effect absent (todo-16) |
| partial-startup-bounded-unwind | M test_session.c:200-216 | Failed host open closes transport | XFAIL, todo 15 | desired observable effect absent (todo-15) |
| missing-disconnect-callback-on-close | M test_session.c:218-251 | Exactly one disconnect callback after lost terminal | XFAIL, todo 15 | desired observable effect absent (todo-15) |
| rejected-dhkey-cannot-claim-foreign-result | T:630-655; review r1 Appendix 3 | Real SC pairing; refused DHKey plus foreign completion cannot emit SMP DHKey Check or Start Encryption | XFAIL, todo 14 | dhkey_checks == 0 && encryptions == 0 (todo-14) |
| foreign-create-busy-control | RV1 A; T test_session.c:675-717, order 0 | Zero Cancel and one independent foreign completion | XFAIL, todo 14 | sim_le_cancels(sim) == 0 && sim_le_foreign_finishes(sim) == 1 (todo-14) |
| foreign-create-success-after-busy | RV1 A; T:675-717, order 2 | Zero Cancel and one independent foreign completion | XFAIL, todo 14 | sim_le_cancels(sim) == 0 && sim_le_foreign_finishes(sim) == 1 (todo-14) |
| foreign-inquiry-untouched | T test_session.c:607-628 | Zero Inquiry Cancel before/during/after busy Inquiry | XFAIL, todo 14 | inquiry_cancels == 0 (todo-14) |
| raw-forbidden-cancel-0402 | T test_session.c:695-700; A2 | Raw Inquiry Cancel rejected without submission | XFAIL, todo 14 | !result && forbidden_commands == 0 (todo-14) |
| raw-forbidden-cancel-0408 | T:695-700; A2 | Raw Create Cancel rejected without submission | XFAIL, todo 14 | !result && forbidden_commands == 0 (todo-14) |
| raw-forbidden-cancel-200e | T:695-700; A2 | Raw LE Create Cancel rejected without submission | XFAIL, todo 14 | !result && forbidden_commands == 0 (todo-14) |
| unacknowledged-connection-Classic-mode-1 | T test_session.c:848-889 | Lost Status cannot authorize Disconnect/connected callback | XFAIL, todo 14 | connection_disconnects == 0 && callbacks.connects == 0 (todo-14) |
| unacknowledged-connection-LE-mode-1 | T:848-889 | Lost Status cannot authorize Disconnect/connected callback | XFAIL, todo 14 | connection_disconnects == 0 && callbacks.connects == 0 (todo-14) |
| late-dhkey-cannot-claim-retired-result | T test_session.c:455-484; review r2 Appendix A | Real SC pairing, real Disconnection Complete and reconnection: a retired link's delayed DHKey Meta completion cannot emit SMP DHKey Check or Start Encryption for the replacement | XFAIL, todo 14 | reuse_checks == 0 && reuse_encryptions == 0 (todo-14) |
| setting-0c01-mode-0 | T test_session.c:921-962, transport refusal | Failed startup closes transport; no cleanup setting traffic | XFAIL, todo 15 | sim_resources(sim).closes == 1 (todo-15) |
| setting-0c01-mode-1 | T:921-962, controller refusal | Same | XFAIL, todo 15 | sim_resources(sim).closes == 1 (todo-15) |
| setting-0c01-mode-2 | T:921-962, lost ACK | Same | XFAIL, todo 15 | sim_resources(sim).closes == 1 (todo-15) |
| setting-0c01-mode-3 | T:921-962, ACK; A4/A8 | No unproven/default mask restoration | XFAIL, todo 13 | setting_cleanup == 0 (todo-13) |
| setting-0c1a-mode-0 | T:921-962, transport refusal | Failed startup closes transport; no cleanup setting traffic | XFAIL, todo 15 | sim_resources(sim).closes == 1 (todo-15) |
| setting-0c1a-mode-1 | T:921-962, controller refusal | Same | XFAIL, todo 15 | sim_resources(sim).closes == 1 (todo-15) |
| setting-0c1a-mode-2 | T:921-962, lost ACK | Same | XFAIL, todo 15 | sim_resources(sim).closes == 1 (todo-15) |
| setting-0c1a-mode-3 | T:921-962, ACK; A4/A8 | No undo without policy/barrier lease | XFAIL, todo 13 | setting_cleanup == 0 (todo-13) |
| setting-0c1c-mode-0 | T:921-962, transport refusal | Zero derived page-scan cleanup commands | XFAIL, todo 13 | setting_cleanup == 0 (todo-13) |
| setting-0c1c-mode-1 | T:921-962, controller refusal | Same | XFAIL, todo 13 | setting_cleanup == 0 (todo-13) |
| setting-0c1c-mode-2 | T:921-962, lost ACK | Same | XFAIL, todo 13 | setting_cleanup == 0 (todo-13) |
| setting-0c1c-mode-3 | T:921-962, ACK; A4/A8 | No undo without policy/barrier lease | XFAIL, todo 13 | setting_cleanup == 0 (todo-13) |
| setting-200c-mode-0 | T:921-962, transport refusal | Zero derived scan-OFF commands | XFAIL, todo 13 | setting_cleanup == 0 (todo-13) |
| setting-200c-mode-1 | T:921-962, controller refusal | Same | XFAIL, todo 13 | setting_cleanup == 0 (todo-13) |
| setting-200c-mode-2 | T:921-962, lost ACK | Same | XFAIL, todo 13 | setting_cleanup == 0 (todo-13) |
| setting-200c-mode-3 | T:921-962, ACK; A4/A8 | No undo without policy/barrier lease | XFAIL, todo 13 | setting_cleanup == 0 (todo-13) |
| failed-setting-rewrite-0c01 | T test_session.c:964-1000 | Lost replacement ACK retires previous mask authority | XFAIL, todo 13 | setting_cleanup == 0 (todo-13) |
| failed-setting-rewrite-0c1a | T:964-1000 | Lost replacement ACK retires scan authority | XFAIL, todo 13 | setting_cleanup == 0 (todo-13) |
| failed-setting-rewrite-0c1c | T:964-1000 | Lost replacement ACK retires page-scan authority | XFAIL, todo 13 | setting_cleanup == 0 (todo-13) |
| failed-setting-rewrite-200c | T:964-1000 | Lost replacement ACK retires LE scan authority | XFAIL, todo 13 | setting_cleanup == 0 (todo-13) |
| refused-scan-no-cleanup | T test_session.c:1002-1025 | Fifty same-time polls issue one attempt, no derived OFF | XFAIL, todo 13 | setting_cleanup == 0 (todo-13) |
| appendix-G-200c-control | RV4 G, nonlate OFF control | Refused ON cannot authorize OFF cleanup | XFAIL, todo 13 | ack_cleanup == 0 (todo-13) |
| appendix-G-200c-delayed | RV4 G:296-331 | Earlier delayed OFF ACK cannot authorize ON cleanup | XFAIL, todo 13 | ack_cleanup == 0 (todo-13) |
| appendix-H-200c-control | RV4 H, nonduplicate control | Refused ON cannot authorize OFF cleanup | XFAIL, todo 13 | ack_cleanup == 0 (todo-13) |
| appendix-H-200c-duplicate | RV4 H:345-389 | Duplicate OFF ACK cannot authorize ON cleanup | XFAIL, todo 13 | ack_cleanup == 0 (todo-13) |
| appendix-G-0c01-control | RV4 G applied to mask, nonlate control | Refused mask replacement cannot authorize restoration | XFAIL, todo 13 | ack_cleanup == 0 (todo-13) |
| appendix-G-0c01-delayed | RV4 G applied to mask | Delayed old mask ACK cannot authorize restoration | XFAIL, todo 13 | ack_cleanup == 0 (todo-13) |
| appendix-H-0c01-control | RV4 H, nonduplicate control | Refused mask replacement cannot authorize restoration | XFAIL, todo 13 | ack_cleanup == 0 (todo-13) |
| appendix-H-0c01-duplicate | RV4 H:345-389 | Duplicate old mask ACK cannot authorize restoration | XFAIL, todo 13 | ack_cleanup == 0 (todo-13) |
| retained-completion-reply | T test_session.c:514-537 | Unrelated completion cannot replace exact read-page-scan response bytes | XFAIL, todo 15 | result && n == sizeof wanted && !memcmp(reply, wanted, sizeof wanted) (todo-15) |
| empty-completion-stream-deadline | RV1 B; T test_hci_usb.c:433-451 | Empty/error stream must end at existing pump timeout, not fixture exhaustion | XFAIL, todo 9 | clock_ms <= deadline && usb.empty_stream (todo-9) |
| refused-USB-init-no-uninit | T test_hci_usb.c:629-636 | Failed USB_FS_INIT cannot authorize USB_FS_UNINIT | XFAIL, todo 9 | usb.uninits == 0 (todo-9) |
| refused-vendor-add-does-not-block-next-add | T test_vpad.c:164-211, failed Add | Failed Add has no identification to drain before another queued Add | XFAIL, todo 10 | adds == 2 (todo-10) |
| failed-startup-transport-cleanup | RV3 F; T test_web.c:152-202 | Actual failed host startup closes transport before reporting | XFAIL, todo 15 | sim_resources(sim).closes == 1 (todo-15) |
| undeliverable-retry-does-not-arm-action | T test_web.c:152-202; review r1 Appendix 1 | Failed host open, caller cleanup, failed response sends and zero ACK bytes cannot arm Retry | XFAIL, todo 11 | g_retry == 0 (todo-11) |
| idle-client-complete-timeout | T test_web.c:236-357; review r1 Appendix 2 | Real idle TCP client receives a completely framed 408 after 3001 virtual ms | XFAIL, todo 11 | complete_response(out, bytes, 408) (todo-11) |

## Hard passing controls and partial ports

No strict error line exists for a hard pass.

| Test/variant | Stimulus source | Observable asserted | Status on M | First strict error |
|---|---|---|---|---|
| LE-Create-ownership-through-terminal, delayed and lost terminal | M:308-346; T:298-339; review B1 | Exactly one Create and successful delivered Status, no terminal before close, zero Cancel/callbacks, one close/uninit within 10 s; delayed terminal reaches system only after release; lost terminal does not appear | Hard pass, both variants | None |
| accepted DHKey and refused DHKey without foreign completion | T:630-655; review r1 Appendix 3 controls | Accepted real SC pairing encrypts and produces usable input; refusal without foreign completion sends zero SMP DHKey Check/encryption/connect | Hard pass | None |
| late-DHKey, old completion also lost; original link retained with its legitimate delayed completion | T:455-484; review r2 Appendix A controls | Two real link lifetimes still produce zero SMP DHKey Check/encryption/connect with no result; the retained original link's genuine delayed completion yields encryption, one connect and usable input | Hard pass, both variants | None |
| foreign-create-success-before-busy; foreign-create-local-status-lost | RV1 A; T test_session.c:675-717, orders 1/3 | Zero Cancel, one independent foreign finish, one close, no callbacks | Pass | None |
| unacknowledged connections, Classic/LE modes 0/2 | T test_session.c:848-889 | Transport refusal/missing terminal: zero Disconnect and connects, one close; LE zero Cancel | Pass | None |
| test_late_link_during_close | T test_session.c:486-512; A4 | Delayed terminal, suppressed Disconnect replies: no unproven Disconnect or callbacks, close inside 10 s, late wire terminal remains independent | Pass | None |
| setting startup failure cleanup traffic | T test_session.c:921-962, first two families modes 0/1/2 | Zero cleanup HCI traffic (transport close is separately XFAIL above) | Pass | None |
| test_refused_scan_does_not_flood_or_acquire_a_lease, pacing half | T:1002-1025 | Fifty same-time polls submit exactly one scan attempt | Pass | None |
| test_refused_transfer_opens_never_authorize_stops, OUT/event/ACL | T test_hci_usb.c:638-649 | No STOP for any unopened transfer; one uninit/close | Pass | None |
| test_node_and_alternate_output_ownership | T test_hci_usb.c:651-668 | Five refused node opens cause no close/uninit; successful alternate OUT gets STOP; refused alternate uses primary | Pass | None |
| test_refused_vendor_libraries | T test_vpad.c:164-211 | Refused library/symbol acquisition returns failure and no Add | Pass | None |
| test_refused_vendor_acquisitions_do_not_invent_ownership, bind/physical halves | T:164-211 | Failed Bind deletes the positively identified Add once, no input; borrowed/refused physical handles never closed | Pass | None |
| test_failed_startup_reports_after_cleanup_deadline, HTTP half | RV3 F; T test_web.c:152-202 | Actual failed startup; caller cleanup; complete State before/after 10001 virtual ms and complete successful Retry | Pass (session-bound F remains below) | None |
| test_client_saturation_and_timeout, saturation/boundary controls | T test_web.c:236-357; review r1 Appendix 2 | Eight real occupied clients; ninth Retry gets complete 429 without arming an action; released slots serve State; idle client stays open at exactly 3000 virtual ms | Hard pass | None |
| test_routes_reply_completely_in_available_states | T test_web.c:236-357 | All 22 expressible route controls plus Retry have exact Content-Length/received-byte agreement and expected status in trying/active/failed states | Pass (session-only states remain below) | None |

## Exact missing-interface obligations

These are not passing tests or silent omissions. They cannot supply the original
stimulus on M without modifying production or inventing a second lifecycle. The
plan explicitly permits this list. The owning todos must add permanent effect
tests when their production interfaces exist. Simulator-only helpers were not
treated as blockers: the foreign-terminal helper was ported above.

| Source/test | Missing production interface on M | Todo inheriting obligation |
|---|---|---|
| RV1 A combined live LE + pending Create + native latch + late DHKey/link + lost terminals | Shared session native/Stop/rest latch; `host_close_with` drain; generation-bearing crypto completion. The standalone late-DHKey/reconnection stimulus is permanent above; the combined single-schedule version needs the shared latch. | 15 (combined), 14 (crypto/link), 16 (native trigger) |
| RV1 C; T test_vpad.c:311-334 `test_stop_before_queued_creation` | `session_t`, `vpad_bind_session`, quiesce/finalization APIs | 10 |
| T test_vpad.c:336-364 `test_release_drains_identification_without_creating_queued_pad` | Same release binding and `vpad_finish` | 10 |
| T test_vpad.c:365-405 `test_poll_read_callback_preempts_add_and_bind` | Session-bound callback latch; generation-safe host admission | 10, with 15 host integration |
| RV2 D; T test_hci_usb.c:488-514 `test_release_inside_read_reap_refuses_raw_submit` | `hci_ops.wait_policy`, host session, transport callback service | 9, with 15 host admission |
| T test_hci_usb.c:516-548 `test_acl_callback_latch_refuses_output` | USB session wait/service callback policy | 9 |
| T test_hci_usb.c:553-598 `test_only_explicit_teardown_can_submit_after_release` | `teardown_cmd`, session release, `host_close_with` and attributable ownership | 9, with 14/15 |
| RV3 E; T test_hci_usb.c:600-627 `test_refused_scan_enable_never_authorizes_cleanup` | Stop before actual USB submission via `wait_policy` | 9, with 13 shared-setting integration |
| RV1 B / T test_hci_usb.c:433-451 absolute session deadline half | Shared absolute session deadline and wait policy (per-call timeout reproducer is permanent above) | 9 |
| T test_hci_usb.c:638-649 command-transfer-open variant | Command transfer endpoint CMD does not exist on M (commands use USB_DO_REQUEST) | 9 |
| T test_vpad.c:164-211 library-finalization assertions | `vpad_finish`/library disposal path; acquisition refusal and device effects are ported above | 10 |
| RV3 F / T test_web.c:152-202 session-bound cleanup expiry only | `host_open_session`, `web_cfg.session`, completed-cleanup reporting state; undeliverable ACK is now runnable above | 11, with 15 failed-startup owner |
| T test_web.c:236-357 starting/quiescing/expired/nonrecoverable routes and Retry-preemption | Shared web session, recoverability/cleanup completion, `web_cfg.service`; real client saturation and request timeout are now runnable above | 11 |
| T test_web.c:359-421 `test_web_callbacks_preempt_pending_actions` | Web service callback plus session-bound host admission | 11, with 15 |
| T test_session.c:342-363 `test_session_policy` | No session module on M | 4 |
| T:392-413 `test_release_during_partial_unwind` | `host_open_session`, failure/release callbacks and fixed shared deadline | 15 |
| T:415-453 `test_rest_in_waits`, startup and raw wait | Same rest latch and shared wait cancellation | 15 |
| T:455-484 `test_late_dhkey_reused_slot`, generation-specific direct-unit half | Generation-bearing crypto owner/link reference type and the direct `link_new`/`dhkey_generation` unit fixture (M has only a bare pointer). The externally expressible late-result/reconnection effects are permanent above. | 14 |
| T:514-537 `test_retained_completion_and_handle_quarantine`, handle half | `link_bind_handle` quarantine, ambiguous release status; reply half is permanent above | 14 |
| T:560-590 `test_repeated_release_during_teardown` | Shared first-reason latch and immutable session deadline | 15 |
| T:750-785 `test_event_callback_preempts_pending_work` | `host_session` release/admission; actual connect callback exists, but cannot express Stop on M | 15 |
| T:787-806 `test_completed_failure_budget_does_not_disable_reporting_or_extend_release` | Session cleanup-completed state, remaining-budget and release transitions | 4, with 15 |
| T:921-962 interrupted / ACK-then-release modes | Session interruption at submission or event-return boundary; four noninterrupting modes are permanent above | 13, with 15 |

`test_le_status_retains_pending_ownership` (T:298-339) retains the inherited
`LE-Create-ownership-through-terminal` identity as two hard effect-level controls.
The old sixth failure asserted only the opposite value of `links[].create_out`.
Under A2's zero-cancel contract, that value does not supply a distinct failing
external oracle for this trace: delayed and absent terminals both leave zero
Cancel/callbacks, close/uninit once within budget, and only the independently
delayed terminal reaches the system after close. The observed successful Status
and absent pre-close terminal rule out accidentally bypassing the original setup.
Thus five inherited failures plus this inherited passing identity are retained;
the plan's literal "six pre-existing XFAILs" cannot honestly coexist with its
effect-only and already-passing-is-hard rules here. No failure is fabricated to
meet a count. Invocation/barrier-qualified lifecycle acceptance remains with 14/15.

## Makefile reconciliation

No target or binary is added to the canonical build. Following the owner's
build-ownership correction, the sole temporary Makefile recipe edit was reversed
with apply_patch. `Makefile`, `ps5.mk` and all of `src/` have an empty diff.
Todo 5 must add two test-only OS redirects to the existing recipe at
`Makefile:60` (target at line 59):

```make
	cc -std=c11 -Wall -Wextra -Werror -O1 -g -Isrc -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Dsocket=test_vpad_socket -Dconnect=test_vpad_connect -o $@ $^
```

Added tokens: `-Dsocket=test_vpad_socket -Dconnect=test_vpad_connect`.
This supplies a pipe-backed kernel-log source for real vpad identification.
All vendor functions remain the existing test executable's stand-ins.

The complete standalone local recipe is exported at
`E/task-2-build.mk`. From the assigned worktree the local equivalent is:

```sh
rm -rf build &&
make -f /home/andres/ps5/build/whateverpad/.omo/evidence/whateverpad-foundation-v2/task-2-build.mk build/test_vpad &&
make test
```

It builds the existing vpad binary first, then lets the unmodified canonical
Makefile build/run everything else. No override warning, compiler wrapper,
canonical build edit or sibling merge is required. A bare clean canonical
`make test` is not claimed green on the final tree until todo 5 integrates
these flags. Root must rerun integrated canonical gates before final acceptance.

## Historical verification receipts (attempt 1, rejected)

These receipts describe `f91c8c6`, not the corrected candidate. They are retained
as history; their 44/2/1/1 ledger is superseded by attempt 2 below.

Final local acceptance passed on the T2 changes plus the separately assigned
web-fixture lifetime repair. Monitor `mon_WMK1168MX3WHTJ6S` / `bash_23`
completed with exit 0. The build-ownership correction is respected: no canonical
Makefile is edited, and integrated canonical acceptance remains with todo 5.

Executed three consecutive times from the assigned worktree:

```sh
rm -rf build &&
make -f /home/andres/ps5/build/whateverpad/.omo/evidence/whateverpad-foundation-v2/task-2-build.mk build/test_vpad &&
make test
```

All three sequences exited **0**. Each `make test` reported 449 session checks
and 659 USB lifetime checks, zero hard failures; unchanged 1244 host, 19 lock
and 43 HearBridge checks; all other existing binaries passed. These are local
equivalent gates, not a bare-clean-canonical gate claim.

After each clean sequence, executed each exact command:

```sh
./build/test_session --expected-failures
./build/test_hci_usb --expected-failures
./build/test_vpad --expected-failures
./build/test_web --expected-failures
```

Every one of the twelve processes exited **1**. Registrations were exactly
**44 session + 2 USB + 1 vpad + 1 web = 48**, with no XPASS or unexpected hard
failure. All six inherited names and all eight G/H names were present in every
run. A resident-kernel audit parsed the actual strict log lines and compared
every name, target todo and exact first error with the table above:
`LEDGER_AUDIT_PASS 48`. All three per-binary ledgers were identical.
Line-buffered stdout prevents stderr controller logs from splitting ledger lines.

`make fuzz` then exited **0**, with ASan/UBSan enabled by the existing target.
Language-server diagnostics on every edited C file were clean; the web fixture
was checked again after its two-line repair. `git diff --check` and
`git diff --cached --check` exited 0. `git diff --exit-code HEAD -- Makefile
ps5.mk src/` exited 0.

The ten METRIC lines were identical across all three final runs and match the
unchanged baseline actually measured in this lane:

```text
METRIC kind=0 case=0 ready=1765 requests=3 cfg=3 hash=7959ca9a connected=1
METRIC kind=0 case=1 ready=1765 requests=3 cfg=3 hash=7959ca9a connected=1
METRIC kind=0 case=2 ready=1765 requests=3 cfg=3 hash=7959ca9a connected=1
METRIC kind=0 case=3 ready=1765 requests=5 cfg=3 hash=3fb1f878 connected=1
METRIC kind=0 case=4 ready=-1 requests=11 cfg=0 hash=b715bea0 connected=0
METRIC kind=1 case=0 ready=1755 requests=3 cfg=3 hash=6d9d25be connected=1
METRIC kind=1 case=1 ready=1755 requests=3 cfg=3 hash=6d9d25be connected=1
METRIC kind=1 case=2 ready=1755 requests=3 cfg=3 hash=6d9d25be connected=1
METRIC kind=1 case=3 ready=1755 requests=5 cfg=3 hash=ddf93ef8 connected=1
METRIC kind=1 case=4 ready=-1 requests=11 cfg=0 hash=b715bea0 connected=0
```

Both artwork hashes remain
`371f06db32d13f3cb2a9cf621ea5e9283ad801c2c4587c5cc90d7318541a4d72`.
Logs are retained only under the assigned worktree's ignored
`.omo/evidence/task-2/`: `supplemental-clean-{1,2,3}.log`,
`supplemental-strict-{1,2,3}-{test_session,test_hci_usb,test_vpad,test_web}.log`,
and `fuzz.log`. None are staged.

Selected final artifact SHA-256 receipts (paths relative to the local log directory):

```text
0d3bba7fec2f0454856a6dc457efb987cd911c7f124823ddf7aead57f32798b3 supplemental-clean-1.log
b6b44a30e4d9631809abc2865e06556cf942c7f6c8d3a251379dbe41306e52f7 supplemental-clean-2.log
a7e69783db083e32a9156eb8fa31ba74c8fc6b498c8abb5d08eb8cb71a3d18b3 supplemental-clean-3.log
4fc9e755bbb4dff43cc37324c334354bfa745914e9931aa8b6a3508eae8034bb supplemental-strict-3-test_session.log
153b32c19b43e9bebc12c8b7f571a77c64ba227f47b2c31b473c3a5e7ff94841 supplemental-strict-3-test_hci_usb.log
779e8ee4340b07a2102277ac6f4ca363fd616092a64d6fb276115d7738da4912 supplemental-strict-3-test_vpad.log
cec79e22e47b1a5f1cf00eb4b2d56fec97ec7507c4a4330fe64053f689b31972 supplemental-strict-3-test_web.log
e71d2ba66c2534fda7dd8d1de2ff7c5deb5fe14f55c72b402eb3dd5afe33fc3d fuzz.log
```

Development classification found and corrected fixture-only issues: a scan ACK
left queued before the replacement fixture, wrong alternate endpoint order,
HTTP page buffer sizing, and EOF processing for truncated test clients.
The old wall-clock `usleep(1000)` request loop was replaced with synchronous
server service and a bounded poll for socket data/EOF. No wall-clock sleeps were
added. The first clean-run launcher was cancelled before acceptance after an
incorrect shell interpolation was noticed; it is not counted as a passing run.
A pre-correction three-run sequence passed with the temporary Makefile edit,
but is not the final acceptance receipt. Runs superseded by the ownership
correction, ledger line-buffering and the owner's newly assigned sanitizer
repair were cancelled and are not counted as final runs. Shared filesystem
journal waits were observed during one cancelled sequence; no timeout or
assertion was suppressed.

No `make sanitize`/`make check-generated` target exists on this base; those gates
activate after todo 5. No hardware behavior or policy verdict is claimed.

## Cleanup and out-of-scope defects

The five inherited effect-level XFAILs and the newly reproduced production defects are not fixed
by this test-only todo. Exact lifecycle stimuli listed above remain obligations
of their named todos, not unverified claims of coverage.
The prohibited inherited private-field assertions are replaced as required by B1;
all earlier effect assertions remain. Temporary failed startup transports
are explicitly closed by the test caller; every new socket, pipe and simulator is
closed/freed. The development web run with the faulty client fixture was cancelled.
The separately assigned inherited web-fixture use-after-free was reproduced
under ASan and repaired without changing assertions or production. Its complete
red/green and cleanup record is `E/task-5-harness-repair.md`.
The T2-only commit intentionally retains that unchanged fixture teardown so the
repair is a separate commit; final local gates above apply to both changes.
Root must integrate both commits and the todo-5 build flags before declaring
canonical acceptance.

## Repair attempt 2: B1-B3 and honest accounting

This is a continuation, not a reset. Parent is `f91c8c6144a8a228583c0cc8b0b037cdc4964c40`;
`4c01c9a` and the separate T5-H1 repair remain untouched. One failed independent
review remains counted; reviewer-classification clarification is still pending.
No delegation, push, merge, main operation, console action or contact occurred.

Before edits, the inherited supplemental suite passed (monitor
`mon_ZZ3KEY0J9C99A02Y`, exit 0). The exact three reviewer appendices were replayed
sequentially against that unchanged tree (monitor `mon_YH4P6JW2YESBE73D`).
All three probe processes exited 1, with these measured effects:

```text
UNDELIVERABLE_RETRY sends=2 bytes=0 retry=1 transport_closes=1
BASELINE_CLIENT_ROWS saturation_429=1 virtual_timeout_408=0
REFUSED_DHKEY attempts=33 smp_dhkey_checks=33 encryptions=0 connects=0
```

B1 now runs real successful Create Status with delayed and lost terminals, with
the hard controls described above. B2 uses actual failed `host_open`, caller
cleanup and `WEB_BT_FAILED`/`cfg.retry`; the test interposes only POSIX `send`.
Ordinary sends forward to real `sendto`; client requests also use real `sendto`,
so the narrow EPIPE refusal affects response delivery only. There is no new
web/session interface or build define. A complete successful Retry before and
after the refused response controls the seam. Eight real idle clients and a
ninth complete 429 are hard assertions; complete 408 at 3001 virtual ms is a
todo-11 XFAIL. Socket readability/EOF is bounded by `poll`; no sleep or timing-luck
loop is added, and readiness descriptors exist before triggering server service.

B3 drives real SC pairing, observes the actual outgoing ACL CID 6/SMP opcode
0x0d and HCI encryption commands, refuses DHKey generation at the command
boundary and injects a foreign completion without forwarding a successful local
generation. A real accepted-pairing control reaches encrypted usable input; the
same refusal without foreign completion emits no continuation. The 1000 virtual
ms negative window is not an assertion of the observed retry count 33. The
accepted control uses the existing LE fixture's 8000 virtual ms window because
GATT's real dropped-frame retry completes after the first 1000 ms.

Focused runs passed normal mode and exited 1 in strict mode. Attempt 2's session
binary reported 515 checks and 44 registered failures: 43 retained real failures
plus the new refused-DHKey failure, rather than the old private-field failure.
Web registered three failures, USB two and vpad one: **50 actual XFAILs**, not
the old 48. Attempt 3 adds the late-DHKey failure, so the current ledger is
**45 / 2 / 1 / 3 = 51** (see the attempt-3 section). The full registered table
above is the exact name/todo/first-error inventory.

### Attempt-2 verification (superseded counts)

Monitor `mon_W4N2VZB3MK237BRY` completed with exit 0. With `MAKEFLAGS` and
`MFLAGS` unset and every make invocation explicitly `-j1`, the following
sequence ran three times, sequentially:

```sh
rm -rf build &&
make -j1 -f /home/andres/ps5/build/whateverpad/.omo/evidence/whateverpad-foundation-v2/task-2-build.mk build/test_vpad &&
make -j1 test
```

All three clean normal suites exited 0. Every suite reported 515 session checks,
659 USB lifetime, 1244 host, 19 lock and 43 HearBridge checks, with zero hard
failures; all other existing binaries passed. After each suite the actual
commands `./build/test_session --expected-failures`,
`./build/test_hci_usb --expected-failures`,
`./build/test_vpad --expected-failures`, and
`./build/test_web --expected-failures` each exited **1**. Exact per-binary
registrations were **44 / 2 / 1 / 3**, total **50**.

The resident-kernel audit read each complete strict log individually, checked
every emitted name, todo tag and exact first error against the table above,
rejected extra/missing/duplicate names and XPASS, and compared all three
per-binary ledgers. Results were `LEDGER_AUDIT_PASS 1 50`,
`LEDGER_AUDIT_PASS 2 50`, `LEDGER_AUDIT_PASS 3 50`.
An initial directory-wide search was detected as per-file truncated and was
discarded; it was not used as an acceptance result. All eight G/H names remain
real failures, including their four controls. The five inherited failing names
remain exact, and the sixth inherited identity appears as two hard passing
wire-effect controls, not as a renamed or unconditional failure.

All ten METRIC lines from each clean run exactly matched the unchanged inherited
baseline and the ten values in the historical block above. Both artwork hashes
still equal `371f06db32d13f3cb2a9cf621ea5e9283ad801c2c4587c5cc90d7318541a4d72`.
`make -j1 fuzz` exited 0: 200000 inputs, 62555 descriptors accepted; bond fuzz
100000 inputs, 1541 valid mutations; no fault.

Targeted ASan/UBSan built and ran the real session, web, USB and vpad test code
with `-fsanitize=address,undefined -fno-sanitize-recover=all
-fno-omit-frame-pointer` and `ASAN_OPTIONS=detect_leaks=1`. Normal exits were
0/0/0/0; strict exits were 1/1/1/1, with exactly 44/3/2/1 registered failures and
no sanitizer findings. Session and web are the two files changed by this repair;
USB and vpad were also checked because they carry the earlier T2 reproducers.

The first extra vpad sanitizer invocation exported the existing global
`dlopen`/`dlsym` vendor stand-ins and exited 139 before producing output. Its
empty `sanitize-vpad.log` is retained, not counted green. The recovery compiled
only that binary with the additional test-only redirects
`-Ddlopen=test_vpad_dlopen -Ddlsym=test_vpad_dlsym`, isolating those stand-ins
from the sanitizer runtime's imports. Existing socket/connect redirects and all
assertions stayed intact; the isolated normal/strict runs then passed with
exits 0/1. No production or canonical build edit was made for this recovery.
Monitors `mon_M07PTF2CRACR1FN1` and `mon_W9T10HK2FV87KFZY` record the failed
first invocation and completed recovery separately.

Language-server diagnostics for both changed C files reported no diagnostics.
`git diff --check` passed. The diff against parent `f91c8c6` is empty for
`src/`, `Makefile`, `ps5.mk`, USB/vpad tests and all simulator files. Only
`tests/test_session.c` and `tests/test_web.c` belong in the corrective commit.
The separately verified T5-H1 teardown ordering is untouched.

### Artifacts and exact commands

All attempt-2 logs and probe sources are retained in the assigned worktree's
ignored `.omo/evidence/task-2/repair-attempt2/`. `commands.md` holds the exact
clean/strict/fuzz/sanitizer launcher commands, including the failed sanitizer
invocation and recovery flags. `ledger-audit.txt` holds the audit results and
all 50 exact final strict first-error lines, grouped by binary. The replayed
review probes are `undeliverable-retry.c`, `client-rows.c`, `refused-dhkey.c`,
with corresponding `repro-*.log` files. Development B1/B2/B3 logs are also kept;
the initial positive SC control's 1000 ms GATT-readiness failure is recorded in
`b3.log`, and the corrected existing 8000 ms window passed in `b3-r2.log`.
No failed development run is counted among the final three clean runs.

Selected final SHA-256 receipts (relative to that artifact directory):

```text
6819e49070f98ab24058caf61739804230f5a239ab8b19931cea63698e5e6972 clean-1.log
3c21429aa3810a89c8bdefb39b22b4af70043e3647f776a48fb3a756907e25b9 clean-2.log
5ec6437a463cd1ba323b6d895adcc0859a0433ce8618483a47c9a9b395422e1d clean-3.log
a4b5a934f20a83891b108cdbffbd90a04118c6e2af58bbfe5e1a5775b1948178 strict-3-test_session.log
153b32c19b43e9bebc12c8b7f571a77c64ba227f47b2c31b473c3a5e7ff94841 strict-3-test_hci_usb.log
caed7915cdf11b98ce05ac3e1b36350d025d6939c886db3e0c46cdf4c6f76ca3 strict-3-test_vpad.log
e6e044dd61686c7e97895017e7080ae785fe72808443fca17d9bbe290d4d2bac strict-3-test_web.log
f345b1af118059d99c9d88985c0b1eb71f5ff62d58a782b439a8c827d639505f fuzz.log
ea3cb5ed78339451c42fadd8d184ae50b10d1a14e0adcf172bacc3757bbdbe25 sanitize-session.log
d9264a5c19b3bb978f61564dadecfe8d33a2902a5bd0aeb3dc6be41547f47bd1 sanitize-web.log
4ddc1a5f8a8bd244312dc47e09e707e08436c912adef9276e909a3f963a8487c sanitize-hci-usb.log
8951a36b381084355e5bda1a32d25f531ac6920786d02939df64acc0ac7cdf44 sanitize-vpad-isolated.log
c53f4f388104487ba8b8be190f45a1ec0da23fb1d79196899fe6f43902e8b95a ledger-audit.txt
956cbd4807041aa16937483595e91360c9dcbf3735ade80daf31a59447178eaa commands.md
```

Final source hashes are `c131b7e42a4a0f3305f72ea0990b9dc352ef12a6ac8f73c2f9c25db5101856fc`
for `tests/test_session.c` and
`a7c1637843eb2c3e9ce61ec23892d36ac793ffa8ed2a25eb967120ef041882dc`
for `tests/test_web.c`.

Canonical clean `make test` integration still requires the existing T5-owned
vpad socket/connect flags. `make sanitize` and `make check-generated` do not
exist on this branch; targeted sanitizer checks do not claim those future
canonical targets. Remaining absent-interface obligations are explicitly listed
above. No hardware acceptance, policy verdict or independent approval of this
new candidate is claimed. The counted failed review and classification question
remain with the orchestrator.

## Repair attempt 3: late-DHKey port, deferral audit and final verification

Continuation from `cffa2a3`, not a reset. B1-B3 are untouched and independently
confirmed. Two failed independent product reviews remain counted; the review cap
and clock are unchanged. Nothing was delegated, pushed, merged, run on main or a
console, nor sent to any person.

### B4 correction

The reviewer's r2 Appendix A stimulus is permanent as
`test_late_dhkey_cannot_claim_retired_result` in `tests/test_session.c`. It
drives real Secure Connections pairing and accepts the first Generate DHKey
(0x2026) normally while holding its real Meta completion for 500 virtual ms.
After that request it powers the simulated peer off, dispatches the real
Disconnection Complete, powers it on and lets the replacement enter through
normal advertising, Create and SMP paths. The replacement's own Meta completion
is always lost. Only delivery of real controller completion events is changed;
commands, pairing, admission and SMP are production code, and no host, link or
DHKey-owner field is read or written. No generation or session interface was
added or faked.

```text
LATE_REUSE mode=0 creates=2 dhkey_requests=2 smp_checks=1 encryptions=1 connects=0 states=0
FAIL late-dhkey-cannot-claim-retired-result: reuse_checks == 0 && reuse_encryptions == 0 (todo-14)
LATE_REUSE mode=1 creates=2 dhkey_requests=1 smp_checks=0 encryptions=0 connects=0 states=0
LATE_REUSE mode=2 creates=1 dhkey_requests=1 smp_checks=1 encryptions=1 connects=1 states=596
```

Mode 0 is the honest todo-14 XFAIL: the retired link's delayed completion still
drives SMP DHKey Check and Start Encryption for the replacement. Mode 1 (old
completion also lost) proves the old result is the causal input: no continuation.
Mode 2 (original link retained, legitimate delayed completion) proves the fixture
still reaches encryption, one connect and usable input. Modes 1 and 2 are hard
assertions. The negative oracle asserts absence of continuation only and pins no
incidental request or retry count.

The previous T:455-484 deferral is narrowed to the generation-specific
direct-unit half (a generation-bearing owner type and T's `link_new`/
`dhkey_generation` fixture). The externally expressible late-result and
reconnection effects are now permanent.

### Deferral audit after B4

Each remaining row was rechecked against its T source and M's headers. The
production interfaces those rows need are absent, not merely unimplemented in
tests: one search over `src/*.c`, `tests/sim.h` and `tests/sim_le.h` for
`link_bind_handle`, `host_close_with`, `session_init`, `session_request`,
`vpad_bind_session`, `vpad_finish`, `wait_policy`, `teardown_cmd`,
`host_open_session`, `web_cfg.service` and `cleanup_pending` returned zero
matches. The handle-quarantine half needs `link_bind_handle`; the policy,
repeat-release and event-callback cases need a session Stop/admission latch and
its shared deadline; the USB cases need a wait policy and explicit teardown
submission; the vpad cases need a release binding and finalization; the web cases
need a session-bound service callback and cleanup-completed state; the
cleanup-budget case is pure session state. None can be expressed through the
current HCI, vendor or socket boundaries without inventing that lifecycle, so
they stay listed obligations for todos 4, 9, 10, 11, 13, 14 and 15. No runnable
effect test was deferred merely because the future ownership implementation is
absent.

### Final verification

Monitor `mon_7VNTRPWFXGWGFNWV` completed with exit 0. `MAKEFLAGS`/`MFLAGS` were
unset, every make invocation was explicitly `-j1`, and runs were sequential to
avoid shared-fixture collisions. Three times:

```sh
rm -rf build &&
make -j1 -f /home/andres/ps5/build/whateverpad/.omo/evidence/whateverpad-foundation-v2/task-2-build.mk build/test_vpad &&
make -j1 test
```

All three clean suites exited 0 with 539 session, 659 USB lifetime, 1244 host,
19 lock and 43 HearBridge checks and zero hard failures. After each suite the
four direct strict commands exited **1**. Exact registrations per run were
**45 / 2 / 1 / 3 = 51**, retaining all six inherited scenario identities (five
failing) and all eight G/H cases. A machine audit read each complete strict log
individually, matched every name, todo tag and exact first error to the table
above, and rejected missing, extra, duplicate and XPASS entries:
`LEDGER_AUDIT_PASS 3x51`, with identical per-binary ledgers across runs.

Ten METRIC lines per clean run were identical to each other and to the unchanged
baseline. `make -j1 fuzz` exited 0 (200000 inputs/62555 descriptors; bond fuzz
100000 inputs/1541 mutations; no fault). Targeted ASan/UBSan builds of the real
session, web, USB and vpad test code ran with `ASAN_OPTIONS=detect_leaks=1`:
normal exits 0/0/0/0, strict exits 1/1/1/1 with exact 45/3/2/1 ledgers and no
sanitizer findings. Artwork hashes remain
`371f06db32d13f3cb2a9cf621ea5e9283ad801c2c4587c5cc90d7318541a4d72`.

Attempt-3 artifacts are retained in the worktree's ignored
`.omo/evidence/task-2/repair-attempt3/`: `clean-{1,2,3}.log`,
`strict-{1,2,3}-{test_session,test_hci_usb,test_vpad,test_web}.log`,
`fuzz.log`, `sanitize-{session,web,hci-usb,vpad}.log`, `sanitize-strict-*.log`,
`b4.log`, `b4-strict.log`, `b4-probe.txt`, `commands.md` and
`ledger-audit.txt`.

```text
c26c9ee469e6cfeadee30eb775d50edf810f49c89e8bf22ea8a11c3ac9ab9a0e clean-1.log
3d82cc907efb73bfaa09e0611962fb5d4955ff9e73ef5c4d0ac2a7facdc19036 clean-2.log
14a5319243f8922d3a5b27fcf2d50bbad99c47dee929c49bcd6bd0a4b7798c35 clean-3.log
43cf32c2e82b6718744c3fc5ab9769b565c4b5a5fc359d651a40cbd5af4c5889 strict-3-test_session.log
153b32c19b43e9bebc12c8b7f571a77c64ba227f47b2c31b473c3a5e7ff94841 strict-3-test_hci_usb.log
f696b84dd04b3627d45f9492667e43ddb8e0ce67fc61e6c5b2ebfb3040e4b37d strict-3-test_vpad.log
c1dc78bd4569c828c75f3249925debfe911851c266c1c00d72df6576de83c05f strict-3-test_web.log
05473d419bd5621f154f0d28f831e4649a15661be70e14ba578e513f4753b5c2 fuzz.log
cae9c9c6d3d8a753bc085957e61a3642f680d9e7c4fdb4c4198574c277d7bf4d sanitize-session.log
3985f44770c5ce9c75d93cff197ca35dbdcf4998ecabdaa68c5c3bbd2361df46 sanitize-web.log
4ddc1a5f8a8bd244312dc47e09e707e08436c912adef9276e909a3f963a8487c sanitize-hci-usb.log
8d21cfd2027cf90e172c49992c2bbe56cde2b57b9ae4664a6aa111a99e820bf0 sanitize-vpad.log
1477937b7cf728d2df866385077715c897b436d0257e293d8fa974d740d916e9 tests/test_session.c
```

### Attempt-3 DoneClaim and cleanup

Corrective commit: **`3cfc702a415645797c408dd0377a26c1fe2f7190`**
(`Cover retired DHKey results across LE reconnection`), sole parent `cffa2a3`,
changing only `tests/test_session.c` (+74). Post-commit status was empty and
`git diff --check` passed. No reset, rebase, amend, push, merge, main edit,
delegation, console action or contact occurred. Probe executables were not
created for this attempt; only logs/source remain, and the sequential runs left
no test process or listener behind.

Language-server diagnostics on `tests/test_session.c` were clean and
`git diff --check` passed. Still unverified: canonical integrated
`make test`/`make sanitize` registration on the wave branch (todo 5), the future
ownership targets, Ubuntu CI, hardware behavior, and independent approval of
this attempt.

### Attempt-2 DoneClaim and cleanup

Corrective commit: **`cffa2a3fe6630d83ac9bd64e8e1923f8381d7117`**
(`Repair effect-only session and web reproducers`).
Its sole parent is `f91c8c6144a8a228583c0cc8b0b037cdc4964c40`; both inherited
commits remain ancestors. It changes only the two verified test files, 192
insertions and 27 deletions. Evidence remains uncommitted as required.

The post-commit `git status --porcelain=v1 --untracked-files=all` was empty;
both working and staged `git diff --exit-code` checks passed. The branch is
still `wave0/t2-reproducers`; it has no configured upstream and nothing was
pushed or merged. No reset, rebase, amend, main edit or console operation ran.

All monitors/runs finished before the DoneClaim. The three private reviewer
probe executables and their object were removed; their source and output logs
remain as evidence. A filesystem check found no executable in the probe artifact
directory, and a process check found no test or sanitizer process remaining.
Normal ignored `build/` outputs are retained for reviewer reuse; no test listener,
debugger or environment override is left running. The session fixtures removed
their temporary directories, and web fixtures closed their sockets and simulators.

Delivered: B1 observable controls and honest six-identity accounting; B2 failed
ACK/Retry and complete timeout regressions plus hard saturation control; B3 real
SC refused-DHKey/foreign-result regression and nonduplicate controls; complete
inventory/strict-ledger corrections; required sequential verification and an
ordinary clean corrective commit. Independent acceptance of this commit is still
the orchestrator's next gate, not an approval claimed by this executor.

T2 commit: `4c01c9aa098bc9a9f88a9c9886ec5391e1dd932e`
(`Add permanent effect-level reproducers for the ownership bug class`).
It contains exactly the six test/simulator files, with no `.omo/`, build-file
or production changes. The separate repair identity is recorded in
`E/task-5-harness-repair.md`.

## B7 preservation controls - 2026-10-23

Starting candidate: `724be2d83a96d0bec760dd70ff7e78c5d50dbe34`,
branch `wave0/t2-reproducers`, initially clean. The first command was
`date -u`, exit 0: `Fri Oct 23 06:20:16 AM UTC 2026`, inside the owner's
`05:23:06Z` to `09:23:06Z` window.

This addition addresses review r4's single blocker B7,
`T2-C2-PORT-AVAILABLE-INVENTORY`. Only `tests/test_vpad.c` and this appended
evidence change. Existing assertions and expected-failure registrations remain
unchanged. The owner's five inherited strict failures plus the faithful
hard-passing sixth identity remain intact; neither new control is an XFAIL.

`test_pending_removal_preserves_unidentified_devices` replays Appendix A using
the existing vendor stand-ins, real HTTP helper, public vpad calls, log pipe
and virtual clock:

| Control | Independent stimulus | External oracle | Baseline status |
|---|---|---|---|
| Zero candidates | Actual successful Add; consume its sole log event before production can identify it; complete real POST Stop; public pending removal; advance 3001 virtual ms | Add/Bind/Delete/input = `1/0/0/0`, also after another remove, poll and attempted input | Hard pass; no strict error |
| Two candidates | Actual successful Add emits `11112222`; append distinct `33334444` before production reads either; complete real POST Stop; public pending removal; drain identification and advance 3001 virtual ms | Add/Bind/Delete/input = `1/0/0/0`, also after another remove, poll and attempted input | Hard pass; no strict error |

The oracle is the vendor-call counters, not production slot, match, generation
or ownership fields. No Bind, Delete or input call is allowed for either
unidentified inventory. The repeated public removal and attempted input cannot
turn ambiguity into authority. HTTP replies are checked completely by the
existing helper; sockets, pipes and credentials are cleaned up. Virtual time
crosses the identification deadline without a sleep.

`make build/test_vpad` and the namespace-isolated `./build/test_vpad` both
exited 0. The output was:

```text
PRESERVATION candidates=0 adds=1 binds=0 deletes=0 frames=0
PRESERVATION candidates=2 adds=1 binds=0 deletes=0 frames=0
all vpad checks passed
3 expected-failing vpad cases
```

### Exact Appendix A mutation demonstration

The exact mutation was applied temporarily to compiler input, as in the
review's own recipe. No tracked production file was edited. From the worktree,
with `set -euo pipefail`:

```sh
F='-std=c11 -Wall -Wextra -Werror -O1 -g -Isrc -Itests -D_DEFAULT_SOURCE'
VF="$F -D_DARWIN_C_SOURCE -Ddlopen=test_vpad_dlopen -Ddlsym=test_vpad_dlsym -Dsocket=test_vpad_socket -Dconnect=test_vpad_connect"
LF="$F -Ddlopen=test_vpad_dlopen -Ddlsym=test_vpad_dlsym"
CORE='src/util.c src/log.c src/crc32.c src/smp_crypto.c src/profiles.c src/generic.c src/bond_store.c src/host.c src/le.c'
before=$(sha256sum src/ps5_vpad.c)
perl -0pe '$n=s/    if \(g_matches != 1\) \{/    if (g_matches != 1) {\n        if (g_matches > 1) scePadVirtualDeviceDeleteDevice((int32_t)g_match_dev);/; die "V5 replacement count $n" unless $n==1;' src/ps5_vpad.c |
  cc $VF -c -o build/b7-mutant.o -x c -
cc $LF -o build/b7-mutant build/b7-mutant.o build/test_vpad_web.o $CORE src/web_page.c tests/test_vpad.c
set +e
env -u MAKEFLAGS -u MFLAGS unshare -Urn -- bash -c \
  'ip link set lo up && ./build/b7-mutant' > build/b7-mutation.log 2>&1
result=$?
set -e
printf 'MUTATION_VPAD_EXIT=%s\n' "$result"
test "$result" -eq 1
test "$before" = "$(sha256sum src/ps5_vpad.c)"
GIT_MASTER=1 git diff --exit-code HEAD -- src
rm build/b7-mutant.o build/b7-mutant
```

Compile and link each exited 0. The mutated permanent suite exited **1**,
with exactly two hard failures, both in the new two-candidate control:

```text
PRESERVATION candidates=0 adds=1 binds=0 deletes=0 frames=0
PRESERVATION candidates=2 adds=1 binds=0 deletes=1 frames=0
  FAIL tests/test_vpad.c:343  adds == 1 && binds == 0 && deletes == 0 && frames == 0
  FAIL tests/test_vpad.c:347  adds == 1 && binds == 0 && deletes == 0 && frames == 0
2 check(s) failed
3 expected-failing vpad cases
```

The zero-candidate control and all previous hard checks still passed. The
runner required exit 1, verified the production SHA-256 was unchanged and the
production diff empty, then removed both mutant artifacts; each check/removal
exited 0. The mutation runner exited 0 only after those requirements held.
The mutation log remains under ignored `build/`; no mutation is committed.
This supplemental mutant was not sanitizer-built.

One initial shell-quoting attempt exited 2 before compiling the mutation; the
corrected compiler-input command above supplied the actual result. The
upstream lookup exited 128 because this local branch has no configured
upstream. Neither diagnostic was treated as a test result or retried for luck.

### Canonical verification recipe

Verification uses the unchanged Makefile recipes, sequentially in one private
user/network namespace. Only that namespace's ephemeral-port range changes,
avoiding the inherited web fixture's PID-derived listener range. The initial
vpad build created `build/` before these log redirects.

```sh
env -u MAKEFLAGS -u MFLAGS unshare -Urn -- bash -c '
set -eu
ip link set lo up
sysctl -w net.ipv4.ip_local_port_range="40000 60999"
make -j1 test > build/b7-test.log 2>&1
ASAN_OPTIONS=detect_leaks=1 make -j1 sanitize > build/b7-sanitize.log 2>&1
for variant in normal san; do
  prefix=build
  if [ "$variant" = san ]; then prefix=build/san; fi
  for suite in test_session test_hci_usb test_vpad test_web; do
    set +e
    ASAN_OPTIONS=detect_leaks=1 ./$prefix/$suite --expected-failures \
      > build/b7-strict-$variant-$suite.log 2>&1
    result=$?
    set -e
    printf "STRICT_%s_%s_EXIT=%s\n" "$variant" "$suite" "$result"
    test "$result" -eq 1
    count=$(grep -c "^XFAIL expected-failing-until-todo-" \
      build/b7-strict-$variant-$suite.log)
    case "$suite" in
      test_session) expected=46;;
      test_hci_usb) expected=2;;
      test_vpad|test_web) expected=3;;
    esac
    test "$count" -eq "$expected"
    if grep -Eq "^XPASS|^  FAIL|ERROR: AddressSanitizer|runtime error:|ERROR: LeakSanitizer" \
      build/b7-strict-$variant-$suite.log; then exit 1; fi
  done
done
'
```

The executed monitor additionally prints and requires each canonical make
exit code to equal 0 before proceeding. Strict exits of 1 are the required
honest failures, not canonical target failures. A separate resident-kernel
audit compares every actual name, todo and first error against the 51-row
historical table plus the three B5/B6 additions, and compares normal and
sanitized ledgers; totals alone are not acceptance evidence.

The first monitor (`mon_P82TTP7W4T0ESKCX`) reached its default 300000-ms
watcher deadline, exit 1, after recording `MAKE_TEST_EXIT=0` and while the
sanitized `test_compat` was running. Its process tree exited; a targeted `ps`
check returned 1 with no matching processes. No completed `make sanitize`
exit is attributed to that interrupted run. Its log is preserved as
`build/b7-sanitize-timeout.log` (`mv`, exit 0). The remaining sanitizer and
strict commands were then started under `mon_RPTRB4XCRXTYXYVR` with an explicit
1800000-ms deadline. The already-passing normal gate was not repeated.

The completed normal log was inspected in full through bounded read chunks:
21559 lines, no hard failure or XPASS, all 54 exact names/todos, both faithful
LE-Create hard-control variants, both new preservation controls and all ten
historical METRIC values. An initial truncated-read audit stopped early and
was discarded; it supplied no missing-coverage finding or test verdict.
Both documented shell recipes also passed `bash -n` with exit 0.

This is test-only B7 coverage, not a fix for the 54 recorded production
failures or a claim of independent gate acceptance. No console, push, merge,
main-branch edit, payload build, fuzz rerun or Ubuntu CI run was performed.

### B7 final verification results

The resumed monitor completed with exit 0. Canonical `make -j1 test` and
`ASAN_OPTIONS=detect_leaks=1 make -j1 sanitize` each completed with exit **0**.
Namespace setup (`unshare`, `ip link set lo up`, and the private `sysctl`)
also succeeded, exit 0. Both new controls hard-pass in normal and sanitizer
builds, including both repeated-removal assertions and attempted input.

| Strict command | Exit | Exact XFAIL count |
|---|---|---|
| `./build/test_session --expected-failures` | 1 | 46 |
| `./build/test_hci_usb --expected-failures` | 1 | 2 |
| `./build/test_vpad --expected-failures` | 1 | 3 |
| `./build/test_web --expected-failures` | 1 | 3 |
| `./build/san/test_session --expected-failures` | 1 | 46 |
| `./build/san/test_hci_usb --expected-failures` | 1 | 2 |
| `./build/san/test_vpad --expected-failures` | 1 | 3 |
| `./build/san/test_web --expected-failures` | 1 | 3 |

Complete receipts were read in bounded chunks. The independent ledger audit
matched every name, todo suffix and exact first error against the existing
tables, rejected extra/duplicate/missing names, XPASS and hard failures, and
compared each normal ledger to its sanitizer counterpart:
`LEDGER_AUDIT_PASS 54 normal + 54 sanitizer`. The five inherited strict
XFAILs and both hard-passing variants of the faithful sixth identity remain.
Only hard-passing coverage increased: the two new controls each observed
Add/Bind/Delete/input `1/0/0/0`.

No hard assertion failure or ASan/UBSan/LeakSanitizer diagnostic occurred in either
completed canonical run. Session/USB/host/lock/HearBridge checks remain
558/659/1244/19/43; the ten historical METRIC lines are identical between
normal and sanitizer runs. Language-server diagnostics on `tests/test_vpad.c`
reported no diagnostics. `git diff --check`, staged-diff checks and protected
path comparisons against the starting candidate exited 0. The C diff has
57 added lines and no removed lines; no existing assertion was changed.

Receipts are `build/b7-test.log`, `build/b7-sanitize.log`,
`build/b7-strict-{normal,san}-{test_session,test_hci_usb,test_vpad,test_web}.log`,
and the mutation/interrupted-run logs named above. Only the two allowed
tracked files are included in the corrective commit; mutant object and binary
are removed, production is byte-clean, and all started verification commands
have finished. B7's previously escaping ambiguous Delete is now detected
without manufacturing an expected failure or changing owner accounting.
