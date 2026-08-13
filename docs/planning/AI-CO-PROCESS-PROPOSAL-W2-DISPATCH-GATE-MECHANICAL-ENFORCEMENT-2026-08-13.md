# AI-CO PROCESS PROPOSAL: Mechanical enforcement of the W2 pre-dispatch gate for shared `dir` workspaces

**Status:** Proposed (analysis + process proposal record of task t_bcde3f19, routed from Coordinator triage t_1481d676)
**Owner:** Process Engineer
**Affected-rule owner:** Main Designer (Hermes dispatcher gate implementation, OM §6 W2 enforcement-point amendment, COORDINATOR_PROFILE note); Coordinator (dispatch gate verification and triage of deferred/blocked cards); Marcel, Human Sponsor (approval of the Hermes runtime modification and any OM amendment)
**Approver:** Marcel, Human Sponsor (adoption of the runtime/OM change); Main Designer (coherence review and implementation of authorized changes)
**Version:** 0.1.0
**Date:** 2026-08-13
**Supersedes:** None (implements the already-adopted W2 enforcement point (b) and Rule W2 text; does not change the normative W1–W4 rule text)
**Review date:** 2026-09-12 (30 days from adoption) or on first recurrence of the covered hazard class, whichever comes first

## 1. Problem and desired outcome

Coordinator triage **t_1481d676** (2026-08-13 14:58–15:04) observed a shared-`dir` running pair and identified one genuine dispatch gap:

- t_6e71a400 (WP-M0-16c1d, senior_specialist) run 865 was ACTIVE in the shared AI-Co tree with uncommitted owned files (`bootstrap/src/ir/ir_builder_stmt.*`, `bootstrap/src/ir/ir_builder_integration_test.c`; mtimes 14:30–14:55) until it timed out at 14:57:58.
- t_7a88311e (planner, S2a-IR planning) was created 14:51:31 with **no parents and status `ready`** (created_by = `worker`), then auto-dispatched 14:52:01 into the same shared tree while 16c1d's uncommitted state was present.
- OM §6 Rule W2: "a second card is not dispatched into that tree until the owning card has committed and its gate is recorded (or the tree is otherwise clean)". The pre-dispatch verification (W2 enforcement point b) is a Coordinator manual procedure; a worker-created, parentless, `ready` card bypasses it mechanically — the dispatcher claims and spawns within one 60-second tick with no tree-cleanliness check.
- Impact was benign because owned paths are disjoint (`docs/planning/` vs `bootstrap/src/ir/`) and the planner committed atomically (113e6db) leaving no uncommitted state; no board-state change was required. But the gate itself was not enforced, and the W2c probe detects the pair only AFTER both are running.

Desired outcome: the W2 dispatch-time tree-cleanliness gate is enforced by the dispatch mechanism itself for shared `dir` workspaces, so that a repo-backed card is not spawned into a tree holding another card's uncommitted tracked changes — independent of who created the card and independent of Coordinator diligence or watchdog timing.

This is a process-control design (Process Engineer domain). Adoption of any Hermes runtime modification or OM/profile amendment is a Main Designer / Marcel decision; this document supplies the analysis, evidence, and the drafted proposal.

## 2. Evidence

All evidence was inspected read-only on 2026-08-13 from the shared kanban DB (`C:\Users\marce\AppData\Local\hermes\kanban.db`), the AI-Co repository, and the Hermes 0.20.0 source checkout (`C:\Users\marce\AppData\Local\hermes\hermes-agent`). Event times are local (+0200).

### 2.1 Incident timeline (triage t_1481d676)

| Time (2026-08-13) | Event | Evidence |
|---|---|---|
| 13:56:54 | t_6e71a400 (16c1d) run 865 starts in shared tree | task_runs run 865 |
| 14:30–14:55 | 16c1d's owned files modified uncommitted in shared tree | filesystem mtimes; triage verification |
| 14:51:31 | t_7a88311e created parentless, status `ready`, `workspace_kind=dir`, path = AI-Co tree, created_by=`worker` | task_events created payload; tasks.created_by |
| 14:52:01 | t_7a88311e claimed (run 866) and spawned by dispatcher — 30 s later | task_events claimed/spawned |
| 14:57:58 | 16c1d run 865 times out | task_runs run 865 (timed_out) |
| 14:58:27 | W2c watchdog probe detects the running pair, creates triage t_1481d676 | task_events created (t_1481d676); kanban_review_router audit_shared_dir_running_pairs |
| 14:59:26–15:04:22 | Coordinator triage runs; CLOSED "contained, no board-state change to source cards"; confirms dispatch gap | task_runs run 868 summary |
| 15:09:28 | Triage archived | task_events archived |

**Interpretation (measurement):** the planner card was dispatched 30 s after creation — within the next 60-second dispatcher tick — while the shared tree held another card's uncommitted tracked changes. The W2c probe fired ~6.5 minutes after the pair began running. Nothing in the create→claim→spawn path checked tree cleanliness.

### 2.2 Dispatch-path mechanism facts

| Fact | Evidence |
|---|---|
| The dispatcher ticks every 60 s (`kanban.dispatch_interval_seconds: 60`) | `config.yaml` (dispatch owner gateway) |
| `create_task` sets a parentless card's status to `ready` immediately (unless `initial_status='blocked'` or `triage=True`); no tree check | `hermes_cli/kanban_db.py` create_task status logic |
| `dispatch_once` reclaims stale → recompute_ready → for each `ready` row: assignee/profile/cap/respawn-guard checks → `claim_task` (ready→running) → `resolve_workspace` → spawn. No git/cleanliness check for `dir` workspaces | `hermes_cli/kanban_db.py` `_dispatch_once_locked` |
| `claim_task` does have a structural claim-time gate precedent: it rejects ready→running while any parent is undone, demoting to `todo` with a `claim_rejected` event | `hermes_cli/kanban_db.py` claim_task |
| The W2c probe (`audit_shared_dir_running_pairs` in `governance/runtime/scripts/kanban_review_router.py`) queries only `status='running'` rows — it cannot observe the pre-dispatch window | kanban_review_router.py lines 317–382 |
| The review-router watchdog runs every 10 min (designer profile cron, interval 10 m) | designer cron jobs.json |
| Current AI-Co tree: no tracked uncommitted changes (`git status --porcelain --untracked-files=no` empty); untracked artifacts (obj/`__pycache__`) are excluded by that flag | git status (2026-08-13 verification) |

**Interpretation:** a router-side pre-dispatch probe polling every 10 minutes cannot reliably beat a 60-second dispatch tick (observed ready→claim window: 30 s). A true pre-dispatch gate must live in the dispatch path itself.

### 2.3 Coverage gap vs existing controls (W2/W2a/W2c)

| Control | State (2026-08-13) | Covers the observed incident? |
|---|---|---|
| W2 dispatch-time cleanliness gate (enforcement point b: Coordinator runs `git status --porcelain` before dispatch; dirty+owned → card stays todo/blocked, never ready) | Manual Coordinator procedure; OM v1.1.10 adopted | **No** — bypassed: worker-created parentless `ready` card, no Coordinator ceremony |
| W2a creation-time structural serialization/isolation for intersecting owned paths (enforcement point a) | OM v1.1.10 + COORDINATOR_PROFILE v1.0.3 adopted; Coordinator verification | **No** — owned paths disjoint (`docs/planning/` vs `bootstrap/src/ir/`), so W2a did not require serialization; also bypassed for worker-created cards |
| W2c mandatory shared-dir running-pair probe (enforcement point d) | Implemented in `kanban_review_router.py` (commit b629ee8), cascade fix 9d96f5f; fired correctly | **Partial** — detected the pair after both were running (14:58:27); cannot prevent the exposure |
| Rule W4 provenance-before-attribution | Adopted | Applies at triage; not a dispatch gate |

**Root cause:** the only designed control for the W2 dispatch-time cleanliness gate is a manual Coordinator step; the card-creation path allows workers to create parentless `ready` `dir` cards that the dispatcher spawns within 60 seconds; the sole automated backstop (W2c) is post-dispatch. This is the same "manual enforcement does not survive normal operation" failure class already classified in the S-1 workspace-isolation proposal (AI-CO-PROCESS-PROPOSAL-SIBLING-WORKSPACE-ISOLATION-2026-08-12.md) — this is its third observed occurrence (16b1 MIN-1/MIN-2 overlap; live S-1 recurrence t_50708e5d+t_95984b78; now 16c1d|S2a-IR).

### 2.4 Alternative explanations considered

- **"The pair's paths are disjoint, so nothing was at risk."** The impact was benign, but the rule W2 regulates the tree state, not only path overlap; the exposure (a second card running in a tree with another card's uncommitted tracked changes) is the exact state the rule forbids, and disjoint-path pairs are not structurally guaranteed to remain disjoint (a planner's docs card can later touch `bootstrap/`; a sibling split can move paths).
- **"The W2c probe is the designed control; post-hoc triage is sufficient."** The probe is a detection/response control and has no prevention role; the S-1 proposal's success measure (a) — zero observations of one card's uncommitted edits appearing in another card's review/verification window — is only enforceable at dispatch time. The observed planner ran ~6 minutes in the dirty tree before detection.
- **"Workers should simply create cards via the Coordinator."** Prose discipline; the org has twice demonstrated prose controls fail under normal operation (S-1 evidence), and worker-created successor cards are a normal pattern here.

## 3. Rule gap vs enforcement gap — assessment

**Conclusion: primarily an enforcement gap; the rule text is sufficient and already adopted.** W2's dispatch-time gate is normative ("a second card is not dispatched into that tree until the owning card has committed and its gate is recorded (or the tree is otherwise clean)"). The failure is that enforcement point (b) is manual, and neither W2a (path-intersection creation check) nor W2c (post-dispatch probe) covers the gap:

- W2a does not apply to disjoint-path pairs and does not run for worker-created cards.
- W2c detects after both cards are running — no prevention.
- The dispatcher — the only actor with the authority and timing to prevent the dispatch — has no tree-cleanliness check.

A creation-time-only fix (e.g., auto-serializing every new `dir` card behind the dirty-tree owner at creation) is insufficient: it evaluates a stale snapshot (the tree can go dirty after creation and before dispatch), it requires the same W4 ownership attribution at a point where less evidence exists, and it cannot catch cards created while the tree is clean. The dispatch-time check subsumes it by evaluating actual state at the moment of spawn.

## 4. Proposed change (hypothesis and measures)

**Affected rule:** OM §6 W2 enforcement point (b) — extend it to note the mechanical dispatcher-side gate; no change to the normative W1–W4 rule text. The Hermes dispatcher (`hermes_cli/kanban_db.py` `_dispatch_once_locked`) gains a config-gated cleanliness gate for `workspace_kind: dir` cards. Complement: an early-escalation probe in `kanban_review_router.py` (read-only, mirror of W2c) for `ready`/deferred `dir` cards whose tree is dirty, giving the Coordinator visibility even when the gate defers a card.

**Normative amendment (draft, for Main Designer to execute and Marcel to approve):**

> **W2 enforcement point (b), extended.** The Coordinator pre-assignment check remains; in addition, the Hermes dispatcher applies a mechanical gate for `workspace_kind: dir` cards before spawn: if the target git tree has tracked uncommitted changes (`git status --porcelain --untracked-files=no`) that are owned by another card that is not terminally done, or whose ownership cannot be established, dispatch is deferred for that tick (card remains `ready`) and a `w2_dirty_tree` event is recorded; after N consecutive defers (default 5 ticks), the card is auto-blocked with reason `w2-dirty-tree: <owner-or-unknown>` for Coordinator triage per Rule W2 ("If ownership cannot be established, dispatch blocks for triage rather than proceeding"). Changes owned by the dispatching card itself (leftover from a prior run of the same card) do not block it. Untracked files never block (build artifacts are not "uncommitted changes" under W2). The gate is opt-in per board via configuration so other Hermes boards are unaffected.

> **W2 enforcement point (d), extended.** The watchdog additionally escalates one deduplicated Coordinator triage card when a `ready` (or dispatcher-deferred) `dir`-workspace card's target tree holds tracked uncommitted changes owned by another active card or of unestablished ownership, so the Coordinator sees pre-dispatch hazards even when the mechanical gate is not (yet) enabled or the card's deferral does not auto-block. Detection and escalation only; the watchdog does not block, unblock, dispatch, or reassign source cards.

**Hypothesis:** requiring the dispatch mechanism itself to defer/block `dir`-workspace cards whose shared tree is dirty under another owner reduces the covered hazard class (second card running in a tree holding another card's uncommitted tracked changes) to zero observed occurrences within the trial period, without material dispatch delay for compliant cards (tree-clean cards dispatch unchanged; self-owned dirt does not block).

**Alternatives considered:**

- **A. No change (rely on W2c + Coordinator triage).** Rejected: third recurrence of the hazard class; W2c is post-hoc and cannot prevent the exposure; contradicts the org's adopted "no manual-only controls" principle.
- **B. Router-side pre-dispatch probe only (escalate/block `ready` dir cards whose tree is dirty).** Rejected as the primary control: 10-minute polling cannot reliably beat a 60-second dispatch tick (observed ready→claim window 30 s); a watchdog write to block a source card is a new dispatch-affecting authority beyond the current read-only charter. Retained only as the complement in enforcement point (d).
- **C. Creation-time structural serialization behind the dirty-tree owner.** Rejected as insufficient: stale snapshot, same W4 attribution need, cannot catch tree-becoming-dirty between creation and dispatch; the dispatch-time gate subsumes it.
- **D. Blanket `worktree` default for all repo-backed AI-Co cards.** Rejected (same reasoning as S-1 alternative B): highest operational cost, conflicts with the manifest's `dir`-kind convention; appropriate as the W2a fallback, not a default.

**Trial scope (bounded):** applies to the Sneedworks shared board's `dir`-workspace cards for 30 days from adoption; dispatcher gate config-gated (enabled for the Sneedworks board only); router probe addition versioned in `governance/runtime/scripts/kanban_review_router.py` with unit tests; no changes to dispatch authority, review class, gate rules, completion rules, or W1/W3/W4 text.

**Owner:** Main Designer (dispatcher gate implementation, router probe, OM §6 enforcement-point amendment, profile note — mechanism owner per OM §6/W3 precedent); Coordinator (triage of `w2-dirty-tree` blocks and verification of gate behavior); Process Engineer (monitoring and evaluation at review date); Marcel (approval of the Hermes runtime modification and the OM amendment).

**Safety limits:** the dispatcher gate never modifies files, never commits, never pushes; it only defers spawns and (after N defers) blocks a card with a typed reason for Coordinator triage; the router complement remains read-only on source cards (creates a triage card + comments only); no destructive action; continuation cap, review gates, acceptance criteria, and owned-path rules unchanged; no secrets; no C: drive writes beyond normal kanban DB events.

**Success measures:** (a) zero new observations of a second card dispatched into a shared `dir` tree holding another active card's tracked uncommitted changes; (b) all `w2-dirty-tree` auto-blocks resolve via Coordinator triage within 24 hours; (c) no regression in review-pass rates or gate evidence quality attributable to the gate.

**Adverse-effect measures:** (a) dispatch/blocked-time delay on deferred cards (track deferred-tick counts and blocked durations vs pre-trial baseline); (b) false-positive defers/blocks (dirty state not owned by another active card, or untracked-only dirt) — must be zero after the untracked-files exclusion and self-owned-dirt exception; (c) gate interactions with the respawn guard / crash-recovery requeue (a card's own leftover dirt must never deadlock its requeue); (d) any Coordinator triage card that turns out to be a non-issue.

**Review date:** 2026-09-12, or on first recurrence of the covered hazard class, whichever comes first. Independent review need: Main Designer coherence review of the dispatcher change and OM text; Reviewer conformance review of the router change per OM §11; Process Engineer preserves negative or inconclusive results.

**Rollback:** disable the config gate (immediate), revert the dispatcher patch (versioned patch file), revert the router probe and OM enforcement-point amendment — all by the Main Designer through the same approval path; trial-period records preserved as evidence per OM §8. The change is low-risk and reversible; the OM amendment is not a constitutional change.

## 5. Implementation handoffs (routing via Coordinator, per OM §6 batch sizing)

| # | Change | Owner | Notes |
|---|---|---|---|
| I1 | Hermes dispatcher gate in `hermes_cli/kanban_db.py` `_dispatch_once_locked` (config-gated, `dir`-only, tracked-only, self-dirt exception, defer→event→auto-block after N) + unit tests; carried as a versioned patch against Hermes 0.20.0 | Main Designer (mechanism owner) | Draft semantics in §4; follow the `claim_task` parents_not_done and respawn-guard patterns |
| I2 | Router complement in `kanban_review_router.py`: early-escalation probe for `ready`/deferred `dir` cards with dirty tree (deduplicated Coordinator triage; read-only on source cards) + unit tests | Main Designer | Mirror `audit_shared_dir_running_pairs`; versioned copy in `governance/runtime/scripts/` |
| I3 | OM §6 W2 enforcement-point (b)/(d) amendment (v1.1.11 change record) and COORDINATOR_PROFILE note for triage of `w2-dirty-tree` blocks | Main Designer (executes; Marcel approves adoption) | Draft text in §4 |
| I4 | Verify gate behavior on the next shared-`dir` dispatch cycle and route any `w2-dirty-tree` blocks to triage | Coordinator | — |

No change is proposed to W1, W3, W4, review verdicts, acceptance criteria, the M0 manifest, or production code.

## 6. Confidence and limitations

- Requirements confidence: High (explicit Coordinator finding in t_1481d676; the incident is the third occurrence of the classified hazard class; the rule W2 text is already adopted).
- Architecture confidence: High (process-control analysis; mechanism is an extension of existing dispatcher checks; no product-architecture impact).
- Verification confidence: High for the incident facts (kanban DB events/runs, filesystem mtimes, router code inspection, config cadence); Medium for the causal attribution "manual enforcement failed because the card bypassed the Coordinator" — supported by created_by=`worker`, parentless `ready` status, and the 30-second dispatch window.
- Limitations: impact was benign (disjoint paths); material harm remains unobserved and is bounded as a risk, not asserted as occurred damage. The Hermes core patch is a runtime modification with upgrade risk; the org runs a pinned local checkout (0.20.0), and a versioned patch file mitigates re-application on upgrades. OM v1.1.10's change record still reads "pending Marcel approval" while adoption was recorded on t_23620782 (2026-08-13) — a stale change-record text the Main Designer/Historian should reconcile.

## 7. Follow-up routing

- Main Designer: I1–I3 (coherence review and execution) per OM §18 / Constitution §8.
- Coordinator: I4 verification; then creation of the I1–I3 cards per OM §6 batch sizing after this proposal's acceptance.
- Historian: index this proposal and the eventual OM v1.1.11 change record.
- Process Engineer: monitor measures at the 2026-09-12 review date; preserve negative results.
- Marcel: approval of the Hermes runtime modification and the OM amendment (adoption path), per the Constitution.

---

**Linked records:** triage t_1481d676 (Coordinator, 2026-08-13); observed pair t_6e71a400 (16c1d, run 865) | t_7a88311e (S2a-IR planning, run 866); prior proposal AI-CO-PROCESS-PROPOSAL-SIBLING-WORKSPACE-ISOLATION-2026-08-12.md (produced W2a); OM v1.1.10 / COORDINATOR_PROFILE v1.0.3 amendments (adopted 2026-08-13, task t_23620782).
