# AI-CO PROCESS PROPOSAL: Artifact-aware block-loop detection for `review-required:` re-review (17c2 false-positive class)

**Status:** Proposed (analysis + process proposal record of task t_a74a593c; source: Main Designer direction 2026-08-14)
**Owner:** Process Engineer
**Affected-rule owner:** Main Designer (OM §6.1 amendment and watchdog implementation; Hermes platform `block_task` loop-breaker change as Hermes runtime owner); Coordinator (triage resolution practice)
**Approver:** Marcel, Human Sponsor (adoption of the OM amendment and any Hermes runtime modification)
**Version:** 0.1.1
**Date:** 2026-08-14
**Supersedes:** None (amends the process governed by OM §6.1 v1.1.10 watchdog rules; does not change review verdict authority, gate rules, or completion rules)
**Review date:** 2026-09-14 (30 days from adoption) or on first recurrence of the covered hazard class, whichever comes first

**Correction record (v0.1.1, 2026-08-14):** per reviewer2 MIN-1 on review t_e09e0754 (verdict PASS WITH MINOR FINDINGS on t_a74a593c @ d2bb309; gate t_b1e46217), the §2.3 board-wide history row for t_b174081d and the interpretation count were corrected: the first blocked event on t_b174081d (08-12 01:54:41) was `Downstream gap: accepted parser (WP-M0-09) rejects missing initializer as AIC-S0101...` (kind needs_input) — NOT a review-required at 24f8f39; the `block_loop_detected` (08-12 03:00:44) fired on the second block (`review-required: WP-M0-13a2 Initializers implemented (commit 24f8f39...)`, kind needs_input) because kind matched while the reason changed completely. Correct classification: **changed-reason false positive** (same class as t_d1a99094). Corrected count: **FOUR of six events (t_31f94221, t_d1a99094, t_b174081d, t_44c69fe9) show changed reason/artifact**; only TWO (t_434a486e, t_d2121da3) are same-commit continuations. Date column corrected for t_d1a99094/t_b174081d (BLD events 08-12, not 08-11); §6 limitation count updated (two, not three, unchanged-commit events). Root-cause conclusion, 17c2 analysis, and proposal content unchanged; text-level correction, no re-review required under the Pass-with-Minors path.

## 1. Problem and desired outcome

The Hermes unblock-loop breaker (`block_loop_detected` event) fired a **false positive** on t_44c69fe9 (WP-M0-17c2) at 2026-08-14 13:54:34, routing the card to `triage` instead of `blocked`, so the OM §6.1 watchdog never created the legitimate post-remediation re-review card. The 17-push chain stalled for **~124 minutes** (13:54:34 → 15:58:23, when the Coordinator manually created the re-review card t_d556cdce + gate t_7c3c7efa and restored the card to `blocked`).

Desired outcome: a card that blocks `review-required:` for a **new artifact** (changed commit / changed reason after an authorized gate deferral) is always routed to the Reviewer as a fresh re-review — never intercepted by the unblock-loop breaker, never triaged by kind alone, and never allowed to stall the pipeline. A **true loop** (the same artifact re-blocked after a CHANGES-REQUIRED verdict with no new progress) still escalates, but with artifact-identity evidence so triage resolves in seconds.

This is a process-control design (Process Engineer domain). Adoption of any OM amendment or Hermes runtime modification is a Main Designer / Marcel decision; this document supplies the analysis, evidence, and the drafted proposal.

## 2. Evidence

All evidence was inspected read-only on 2026-08-14 from the shared kanban DB (`C:\Users\marce\AppData\Local\hermes\kanban.db`), the AI-Co repository, and the Hermes 0.20.0 source checkout (`C:\Users\marce\AppData\Local\hermes\hermes-agent`). Event times are local (+0200).

### 2.1 Where the loop guard actually lives (verified — corrects the diagnosis target)

The task direction assumed the guard lives in `governance/runtime/scripts/kanban_review_router.py`. **It does not.** The Sneedworks-owned router creates review/gate cards and comments; it never changes task status. The `block_loop_detected` interception is in the **Hermes platform core**, `hermes_cli/kanban_db.py` `block_task()`:

| Fact | Evidence | Source |
|---|---|---|
| Guard lives in `block_task()`, not the router | `block_task` sets status to `triage` and appends the `block_loop_detected` event with payload `{"reason": ..., "kind": ..., "recurrences": N, "limit": 2}`; the router contains no `block_loop_detected` string and only reads `status='blocked'` rows | `hermes_cli/kanban_db.py` lines 5727–5765; `kanban_review_router.py` `main()` lines 1134–1137 |
| Same-cause test is **kind-only** | `same_cause = prev_kind == kind`; `recurrences = prev_recurrences + 1 if same_cause else 1` — the block **reason/artifact is never compared**, only the block kind (`needs_input`/`capability`/`transient`/None) | `kanban_db.py` lines 5724–5725 |
| Limit is hardcoded at 2 | `BLOCK_RECURRENCE_LIMIT = 2` (module constant; no config key found in `config_defaults.py`) | `kanban_db.py` line 134 |
| Unblock does NOT reset the counter | `unblock_task` deliberately leaves `block_recurrences` and `block_kind` intact (anti-amnesia design against cron unblock loops); only `complete_task` resets them | `kanban_db.py` lines 5942–5951, 4918, 4935 |
| Triage happens BEFORE the watchdog runs | The router scans only `status='blocked'`; a card routed to `triage` by the loop breaker is invisible to review routing, so no re-review card is created | `kanban_review_router.py` `main()` lines 1134–1137 |
| The 16c1c chain's legitimate re-review rounds were on **separate cards** | origin t_66f6be4e @ 7c43489; remediations t_85b54d7c @ c391dbc, t_83fa0e82 @ 2abd771, t_9ee2aaab @ e61c496 — each card blocked `review-required:` exactly once, so kind-only counting never fired there | task_events `blocked` payloads |

**Interpretation (mechanism):** the breaker's design goal is stopping a cron/agent unblock → same-cause re-block spin. But `same_cause` is implemented as *same kind*, not *same cause*. Any second `needs_input` block after an unblock counts as a recurrence regardless of whether the underlying artifact changed. For the org's review gate, a second `review-required:` block on the same card is a normal, legitimate workflow step whenever a gate defers completion pending remediation (PASS-with-minors-DEFERRED) — exactly what happened on 17c2.

### 2.2 The 17c2 false positive (verified timeline)

| Time (2026-08-14) | Event | Evidence |
|---|---|---|
| 10:35→11:10 | Run 2: t_44c69fe9 completes at commit 0ad8411, blocks `review-required:` (kind=needs_input, recurrences=1) → routed | task_events blocked 1786698631 |
| 11:17 | Watchdog routes review t_afd29581 + gate t_a946c222 | router comment 1786699065 |
| 11:35 | reviewer2 VERDICT PASS WITH MINOR FINDINGS (0C/0M/2Minor/2Sug) @ 0ad8411 | review card comment 1786700128 |
| 11:42 | Coordinator gate t_a946c222: PASS-with-minors, **terminal completion DEFERRED** per Planner RULING 3 (t_7197bb24); explicit directive to re-block `review-required:` after post-remediation evidence; card unblocked to `todo` on structural parent edge (t_79ffdc51) | gate card + card comment 1786700536 |
| 13:15 | 17c1 remediation t_79ffdc51 terminally done @ 39fb443 | parent completion |
| 13:16→13:54 | Run 3: specialist posts post-remediation closure evidence at **NEW commit 8c2d3fb** (multi-site-in-one-function + MIN-2 coverage; 403 checks/0 failures) and blocks `review-required:` again | comment 1786708469; blocked 1786708474 |
| 13:54:34 | **`block_loop_detected` fires** — `same_cause = prev_kind == kind` (needs_input == needs_input) → recurrences=2 → ≥ 2 → status=`triage`; the router never sees a `blocked` card, so no re-review card is created | task_events block_loop_detected 1786708474, recurrences=2, limit=2 |
| 13:54→15:58 | Pipeline stalled; push wave t_3125ecb2 and 18a1 (t_d0c9a826) parented on 17c2 done cannot fire | task_links |
| 15:45→15:58 | Coordinator triage t_20a65a43 verifies FALSE POSITIVE, creates re-review t_d556cdce @ 8c2d3fb + gate t_7c3c7efa, restores card to `blocked` (block_recurrences reset to 1) | triage card; card comment 1786715968; blocked 1786715998 |

**Interpretation (measurement):** the guard compared only the block kind. The artifact changed (0ad8411 → 8c2d3fb), the reason changed, and the prior gate disposition explicitly authorized the re-block — none of that was consulted. The interruption cost was ~124 minutes of pipeline stall plus a manual Coordinator triage that duplicated what the watchdog would have done in minutes.

### 2.3 Board-wide history: the same class repeats (6 of 6)

| task_id | BLD time | BLD kind / reason vs prior blocked reason | Class |
|---|---|---|---|
| t_434a486e (WP-M0-08 lexer) | 2026-08-10 | review-required e1929bc re-blocked after continuation (same kind, same commit, re-verified) | kind-only false positive; human-resolved; card done |
| t_d2121da3 (WP-M0-09 parser) | 2026-08-10 | review-required 70efe05 re-blocked after continuation (same kind, same commit, re-verified) | kind-only false positive; human-resolved; card done |
| t_31f94221 (WP-M0-03-fix2→fix3) | 2026-08-11 | reason **changed** fix2 → fix3 (new remediation round), same kind | **changed-artifact false positive; card still in triage** |
| t_d1a99094 (OM v1.1.7 adoption) | 2026-08-12 | reason class changed `human-required:` → `review-required:` (Marcel gate passed, then review requested), same kind | **changed-reason false positive; resolved via triage; archived** |
| t_b174081d (WP-M0-13a2) | 2026-08-12 | first block `Downstream gap: accepted parser (WP-M0-09) rejects missing initializer...` (needs_input); BLD fired on second block `review-required: 24f8f39` (same kind, reason changed completely) | **changed-reason false positive; human-resolved; card done** |
| t_44c69fe9 (WP-M0-17c2) | 2026-08-14 | review-required 8c2d3fb after gate DEFERRED on 0ad8411 (changed artifact, same kind) | **changed-artifact false positive; 124-min stall** |

**Interpretation:** all six observed `block_loop_detected` events fired at `recurrences=2, limit=2`; none is a demonstrated cron-spin loop. **FOUR of six (t_31f94221, t_d1a99094, t_b174081d, t_44c69fe9) show changed reason/artifact between the two blocks**; only TWO (t_434a486e, t_d2121da3) are legitimate same-commit continuation re-verifications that the guard also interrupted. The kind-only `same_cause` test is the common root cause. The guard's anti-loop purpose is not in dispute — the signal it uses is wrong.

### 2.4 Existing artifact-identity machinery (the model to reuse)

The Sneedworks router already has the identity comparison the loop breaker lacks:

| Function | Behavior | Location |
|---|---|---|
| `extract_commit(text)` | first `\b[0-9a-f]{7,40}\b` in the reason/body | kanban_review_router.py 385–389 |
| `artifact_identity_matches(review_body, reason, commit)` | same commit, or same reason text within the 400-char embed limit when no commit is named | kanban_review_router.py 175–184 |
| `covering_completed_cycle(con, blocked, reason, commit)` | completed review+gate cycle covering the same artifact | kanban_review_router.py 205–229 |
| OM §6.1 v1.1.5 principle | "A changed reason identifying a new artifact always routes fresh" | OPERATIONS_MANUAL.md line 163 |

The router already implements artifact-aware dedup for the *covered-cycle* path; the loop breaker bypasses it because the breaker fires in the platform before the router runs.

### 2.5 Alternative explanations considered

- **"17c2 was a one-off; raising the limit to 3 fixes it."** Rejected: the recurrence counter is per-card and only resets on completion; a legitimate multi-round review chain on one card (origin + N remediation rounds, each re-blocking `review-required:`) would hit any fixed limit. 16c1c's three rounds only escaped because each round was a separate card. Raising the limit delays but does not remove the false positive; it also weakens genuine-loop protection. Artifact-awareness is the required signal, not a higher count.
- **"The Coordinator gate should have completed the card instead of deferring."** Rejected: the deferral (Planner RULING 3) was correct and authorized — the card could not be terminally completed before the sibling remediation landed. The failure is the guard's, not the gate's.
- **"The router should unblock triaged cards."** Rejected: the watchdog never unblocks/completes (OM §6.1); changing that would expand watchdog authority. The compensating control routes in parallel (direction #3) and leaves triage-state resolution to the Coordinator.

## 3. Rule gap vs enforcement gap — assessment

**Conclusion: primarily a platform-mechanism gap with a compensating router gap.** OM §6.1's watchdog wording is artifact-aware ("a changed reason identifying a new artifact always routes fresh"), but the platform loop breaker runs first, on kind alone, and can intercept the card before the watchdog's artifact logic ever sees it.

- **Platform gap (root cause):** `block_task`'s `same_cause = prev_kind == kind` ignores the reason/artifact. No OM wording can fix this; only a change to the platform function (or a compensating control that routes despite the triage) can.
- **Router gap (compensating):** the router scans only `status='blocked'`; a card triaged by the breaker is invisible to review routing, and there is no path that creates the review+gate pair for a triaged `review-required:` card, nor evidence for the Coordinator to resolve the false positive in seconds.
- **Threshold:** `BLOCK_RECURRENCE_LIMIT=2` is too aggressive *as a kind-count* (every second review-required fires), but with artifact-aware reset, a same-artifact threshold of N=3 (allowing two same-artifact re-requests before triage) preserves genuine-loop protection while absorbing the observed legitimate patterns.

## 4. Proposed change (hypothesis and measures)

**Affected rules:** OM §6.1 blocked-review routing / watchdog mechanism (v1.1.10), and the Hermes platform `block_task` unblock-loop breaker (Hermes runtime, `hermes_cli/kanban_db.py`).

### 4.1 Normative amendment (draft OM §6.1.x rule, for the Main Designer to execute and Marcel to approve)

> **§6.1.x — Block-loop guard and legitimate re-review (artifact-aware).**
> A `review-required:` or `remediation-required:` block is a request for the Reviewer role (OM §6.1). The Hermes unblock-loop breaker (`block_task`, `BLOCK_RECURRENCE_LIMIT`) is a platform mechanism whose purpose is to stop unblock → same-cause re-block spins; it must not intercept a legitimate re-review.
>
> 1. **Artifact identity is the cause signal.** When a card blocks `review-required:`/`remediation-required:` and a prior block or review exists for that card, the cause is the artifact identity: the commit SHA named in the block reason (fallback: the newest commit on the card's owned paths per Rule W3/W4 provenance), not the block kind.
> 2. **Changed artifact → reset and route fresh.** If the artifact changed since the last review verdict (different commit, or a changed reason naming a new artifact per OM §6.1 v1.1.5), the recurrence counter is reset (recurrences=1), the block is treated as a new cause, and the watchdog routes it normally — the loop breaker never triages a changed-artifact re-review.
> 3. **Same artifact + no new progress → genuine loop.** If the artifact is unchanged (same commit / same reason) after a CHANGES-REQUIRED verdict and the card re-blocks repeatedly, the loop breaker escalates at N=3 recurrences with evidence: last reviewed commit, current commit, gate disposition, owned-path progress.
> 4. **Gate-disposition awareness.** A prior gate application that was PASS-with-minors-DEFERRED, or any gate that explicitly directs "re-block review-required after remediation," authorizes the next `review-required:` block; that disposition suppresses loop escalation for the next block.
> 5. **Prefer routing over intercepting.** If a loop is suspected, the watchdog still creates the re-review/gate pair (or escalates with routing already done) so a false positive never stalls the pipeline; the Coordinator resolves any triage-state restoration. The watchdog never unblocks or completes the card itself.
> 6. **Evidence in the escalation.** When the breaker does fire, the escalation includes the comparison: last reviewed commit, current commit, gate disposition, and owned-path progress, so triage resolves in seconds.

### 4.2 Platform change (root cause; Hermes runtime — Main Designer as runtime owner, Marcel approval)

In `hermes_cli/kanban_db.py` `block_task()`:

- Replace the kind-only cause test with an artifact-aware test:
  - fetch the previous block's reason (latest `blocked` event payload for the task before this call);
  - compute the artifact identity: `extract_commit(reason)` (first `\b[0-9a-f]{7,40}\b`) if present, else the normalized reason text;
  - `same_cause = (prev_kind == kind) AND (artifact_identity(prev_reason) == artifact_identity(reason))`.
- Raise `BLOCK_RECURRENCE_LIMIT` from 2 to 3 and make it configurable via an environment knob (e.g. `KANBAN_BLOCK_RECURRENCE_LIMIT`, default 3) so the org can tune without a source patch.
- Keep the deliberate no-reset-on-unblock behavior (it protects against cron loops); artifact change now resets the counter by the identity test.

Unit tests (Hermes platform test suite `tests/hermes_cli/test_kanban_block_kinds.py`):
- **False-positive regression:** `test_second_review_required_changed_commit_not_escalating` — block(reason A @ commit C1, kind needs_input) → unblock → block(reason B @ commit C2, kind needs_input) must NOT emit `block_loop_detected`; task lands `blocked`, recurrences=1.
- **True-loop regression:** `test_same_commit_repeated_review_required_escalates_at_limit` — block(reason @ commit C) → unblock → block(same reason @ commit C) → unblock → block(same reason @ commit C) must emit `block_loop_detected` at recurrences=3 and route to `triage`.

### 4.3 Router compensating control (org-owned, implementable immediately)

Add to `governance/runtime/scripts/kanban_review_router.py` a new sweep `audit_triaged_loop_blocks(con)` (called from `main()`), mirroring the existing escalation patterns:

- Query `triage` cards having a `block_loop_detected` event (task_events kind) whose latest block reason starts with `review-required:`/`remediation-required:`.
- For each: compute the artifact identity; if the artifact is **not** covered by a completed review/gate cycle (`covering_completed_cycle` returns None) and no live review card exists for it:
  - **route the re-review anyway** via the existing `ensure_routing` (review card + gate card + marker), so the loop guard never blocks the re-review card;
  - post a `AIC-WATCHDOG-LOOP-FP` marker comment with the evidence comparison (last reviewed commit, current commit, gate disposition excerpt) and the triage card reference for Coordinator restoration.
- If the artifact **is** covered (same artifact) → genuine loop candidate: leave for Coordinator, post the evidence marker only (no new pair) so the Coordinator sees the identity comparison.
- Never unblock, complete, reassign, or change the triaged card's status; the Coordinator restores the card to `blocked` after routing (the 17c2 recovery pattern).

Unit tests (`governance/runtime/scripts/test_kanban_review_router.py`):
- **False-positive case:** `test_triaged_review_required_new_commit_still_routes_pair` — triaged card with `block_loop_detected` + `review-required:` naming commit C2, prior review covered C1 → sweep creates review+gate pair and posts the LOOP-FP marker; card stays `triage`.
- **True-loop case:** `test_triaged_review_required_same_commit_no_new_pair` — triaged card with `block_loop_detected` + `review-required:` naming the same commit already covered by a completed cycle → sweep posts the evidence marker only, no new pair.
- **Covered-cycle identity reuse:** `test_changed_commit_routes_fresh_pair` already covers the router-level fresh-routing logic; extend to the triage path.

### 4.4 Hypothesis

Requiring artifact-identity comparison in the loop breaker, plus a router compensating control that routes re-reviews even for triaged cards, reduces the covered hazard class (false-positive `block_loop_detected` interception of legitimate re-reviews) to zero observed occurrences within the trial period, without weakening genuine-loop protection (same-artifact, no-progress loops still escalate at N=3 with evidence).

**Alternatives considered:**
- **A. No change (Coordinator triage absorbs false positives).** Rejected: six occurrences, one 124-minute stall, one card still sitting in triage (t_31f94221); manual triage does not survive normal operation.
- **B. Raise the limit to 3 and stop.** Rejected (see §2.5): delays but does not remove the false positive; weakens genuine-loop detection.
- **C. Router-only compensating control.** Implemented as §4.3 and is the immediate org-owned mitigation; does not fix the platform root cause, so it is proposed together with §4.2, not instead of it.
- **D. Make the watchdog reset the counter via direct DB writes.** Rejected: the deployment guide forbids patching Kanban SQLite control stores as a normal operating method; the platform change is the sanctioned path.

**Trial scope (bounded):** AI-Co cards blocking `review-required:`/`remediation-required:` for 30 days from adoption; watchdog change implemented in `kanban_review_router.py` (versioned copy in `governance/runtime/scripts/`); platform change implemented by the Main Designer as Hermes runtime owner with Marcel approval; no changes to review class, verdict authority, gate rules, completion rules, or WIP limits.

**Owner:** Main Designer (OM §6.1 amendment text and watchdog implementation, I2; Hermes platform change, I3); Coordinator (triage-resolution practice and any immediate card restorations); Process Engineer (monitoring and evaluation at review date); Marcel (approval of the OM amendment and Hermes runtime modification).

**Safety limits:** the watchdog compensating control is read-only with respect to card state — it only creates cards/comments (no unblock, no complete, no dispatch change, no status change); the platform change keeps the anti-amnesia no-reset-on-unblock design and only changes the cause signal; no destructive action is authorized; no owned-path or acceptance-criteria changes; no C: drive writes; no secrets; never push.

**Success measures:** (a) zero new false-positive `block_loop_detected` interceptions of changed-artifact `review-required:` re-reviews; (b) every triaged `review-required:` card with a changed artifact gets its review+gate pair within one watchdog sweep (~10 min); (c) genuine loops (same artifact, no progress, N recurrences) still escalate to triage with evidence; (d) no regression in review-pass rates or gate evidence quality.

**Adverse-effect measures:** (a) genuine loops that escape triage longer because the threshold rose to 3 (track time-to-triage on any same-artifact re-block); (b) duplicate review cards if the compensating control races the platform fix (idempotency via existing review-card existence checks and idempotency keys); (c) Coordinator triage cards that turn out to be non-issues; (d) any platform-patch instability after a Hermes upgrade (track Hermes version at adoption and at review date).

**Review date:** 2026-09-14, or on first recurrence of the covered hazard class, whichever comes first. Independent review need: Main Designer coherence review of the OM text; Reviewer conformance review of the watchdog code change per OM §11; Reviewer review of the Hermes platform change per the Hermes Deployment Guide; Process Engineer preserves negative or inconclusive results.

**Rollback:** revert the OM §6.1 amendment and version bump; revert the router sweep (versioned script copy); revert the platform `block_task` change and restore `BLOCK_RECURRENCE_LIMIT=2` — all by the Main Designer through the same approval path; trial-period records preserved as evidence per OM §8. The change is low-risk and reversible; adoption may proceed without a Constitution §8 temporary exception (not a constitutional change).

## 5. Implementation handoffs (routing via Coordinator, per OM §6 batch sizing)

| # | Change | Owner | Notes |
|---|---|---|---|
| I1 | OM §6.1 amendment (new §6.1.x artifact-aware rule) with v1.1.11 change record | Main Designer (executes; Marcel approves adoption) | Draft text in §4.1 above |
| I2 | Watchdog compensating control `audit_triaged_loop_blocks` in `kanban_review_router.py` + unit tests (false-positive routes pair; same-artifact true loop posts marker only) | Main Designer (watchdog mechanism owner) | Follow the existing stale-escalation and shared-dir probe patterns; never changes card status |
| I3 | Hermes platform `block_task` artifact-aware cause test + `BLOCK_RECURRENCE_LIMIT` 2→3 (configurable) + platform unit tests | Main Designer (Hermes runtime owner); Marcel approval | §4.2; version-check the Hermes install before/after |
| I4 | Immediate operational: resolve any currently-triaged false-positive `review-required:` cards (e.g. t_31f94221) by routing the re-review and restoring `blocked`, mirroring the 17c2 recovery pattern | Coordinator | Live at time of writing |

No change is proposed to review verdicts, acceptance criteria, the M0 manifest, or production code. The OM §6.1 v1.1.5 "changed reason routes fresh" principle is strengthened by enforcement, not rewritten.

## 6. Confidence and limitations

- Requirements confidence: High (explicit Main Designer direction; verified mechanism and timeline).
- Architecture confidence: High (process-control analysis; platform change is a small, well-scoped edit to one function plus a constant).
- Verification confidence: High for the mechanism facts (DB events, source lines, six board-wide instances); High for the 17c2 stall duration (124 min measured between BLD event and manual re-review card creation).
- Limitations: the platform change requires modifying the Hermes install, which is outside the Sneedworks repo; the org's versioned copy pattern covers the router only. The compensating control (§4.3) is the org-owned mitigation that works even before the platform change lands. Two of six observed events had *unchanged* commits between blocks (continuation re-verifications) — the artifact-identity test treats those as same-artifact and would still let the breaker fire at N=3; if continuation re-verifications are a distinct legitimate class, a follow-up may exempt them via a separate signal (e.g. circuit-breaker continuation evidence per Rule W3). This limitation is recorded for the review date.

## 7. Follow-up routing

- Main Designer: I1–I3 (coherence review and execution) per OM §18 / Constitution §8; Hermes runtime change per the Hermes Deployment Guide.
- Coordinator: I4 immediate triage-state resolutions; then creation of the I1–I3 cards per OM §6 batch sizing.
- Reviewer2: independent review of this proposal (task-level review per the S-1 flow), then conformance review of I2 and I3 changes.
- Historian: index this proposal and the eventual OM v1.1.11 change record.
- Process Engineer: monitor measures at the 2026-09-14 review date; preserve negative results.
- Marcel: approval of the OM §6.1 amendment and the Hermes runtime modification (adoption path), per the Constitution.
