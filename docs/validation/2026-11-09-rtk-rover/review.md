# RTK rover fix-wave re-review

**Spec compliance: PASS. Code quality within the reviewed changes: PASS.** All five findings from `final-review.md` are addressed. No additional actionable regression was identified in `fix-diff.txt`.

| Original finding | Resolution checked against code and regressions |
| --- | --- |
| RTCM replay at start, resume or PX4 recovery | The sole base reader drains input under its existing budget, excludes preactivation bytes including a partial frame across the boundary, and processes UBX ACK and NMEA normally. The service drops the batch read during a not-ready → ready transition. The PTY cases cover unread and crossing input, initial verification, heartbeat recovery and the next eligible frame. |
| Rejected resume executing on retry | The controller caches the authorized operational rejection and records its lease. The regression changes the relay to paused, redelivers the original ID, and checks the unchanged rejection and paused state. |
| `forwarding` beyond the relay freshness limit | Both the send decision and reported state honor the 1,000-ms RTCM cutoff. The regression presents a still-fresh base receiver with two-second-old corrections and checks `waiting_rtcm`. |
| Transition history leaking across experiments; elapsed time advancing after stop | Service status selects transitions strictly after start and through stop, tags the selected entries with the current run and elapsed time, and keeps the end time on repeated stops. Live GPS continues between experiments. A retained transfer carries its original run sequence. |
| Duplicate GPS source timestamp producing conflicting evidence | Duplicate measurements update receive age only. The generic GPS store, accepted raw bytes, fix, sample sequence and sample clock advance together for an accepted measurement. Session and service regressions exercise different fix values under the same source timestamp and verify the accepted wire bytes. |

Reviewed the supplied RED/GREEN report and regression code without rerunning tests. The reported scoped WSL result is 4/4 executables, 60 cases and 1,258 assertions with no compiler warnings. The fixed-size transition window and its eviction counter remain connection-scoped; the service filters the displayed history to the selected run. This review covers the Pi software changes. Deployments, radio delivery and outdoor fix/convergence remain distinct validation activities.
