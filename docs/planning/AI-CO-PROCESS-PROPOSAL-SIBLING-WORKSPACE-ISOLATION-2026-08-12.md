# AI-CO PROCESS PROPOSAL: Isolate or serialize sibling tasks sharing a source area (reviewer2 S-1)

**Status:** Proposed (analysis + process proposal record of task t_50708e5d)
**Owner:** Process Engineer
**Affected-rule owner:** Main Designer (Operations Manual §6 amendment and watchdog implementation, COORDINATOR_PROFILE); Coordinator (card-creation and dispatch practice); Planner (manifest serial-discipline wording where it applies)
**Approver:** Marcel, Human Sponsor (adoption of any Operations Manual amendment); Main Designer (coherence review and implementation of authorized changes)
**Version:** 0.1.1
**Date:** 2026-08-12
**Supersedes:** None (amends the process governed by OM §6 W1–W4 v1.1.7 as adopted; does not change the normative W1–W4 rule text itself)
**Review date:** 2026-09-11 (30 days from adoption) or on first recurrence of the covered hazard class, whichever comes first

**Correction record (v0.1.1, 2026-08-12):** per reviewer2 MIN-1 on review t_997df7f5 (verdict PASS WITH MINOR FINDINGS on t_50708e5d), the S-1 citation was corrected: review card **t_9ac8e485 (comment 1279) is the sole S-1 source for this workspace-isolation hazard**; t_47cce3e7's S-1 (comment 1274) concerns ir_dump_verify invariant-12 test coverage — a different suggestion, not a source of this hazard. Corrected at §1, §2.1 evidence-table row, §2.4, and §6.

## 1. Problem and desired outcome

Reviewer2's Suggestion S-1 (review card t_9ac8e485 verdict comment 2026-08-12 20:19 — the sole S-1 source for this hazard; routed via the Coordinator to the Process Engineer) identified an evidence-integrity hazard: sibling follow-up cards (MIN-1 t_171e3b7f and MIN-2 t_30ae127a, both children of WP-M0-16b1 t_19837659) share the same `dir` workspace (`E:\Hermes_Agent\projects\Sneedworks\projects\AI-Co`) and their execution/review windows overlapped while both cards were editing the same source files (`bootstrap/src/ir/ir_core.c`, `bootstrap/src/ir/ir_core_test.c`). During reviewer2's independent re-review of MIN-1, MIN-2's in-progress UNCOMMITTED edits appeared in the shared working tree (mtime 20:16 local). The verdict was unaffected only because the reviewer verified from an isolated clean worktree at edc8db7. (Note: review card t_47cce3e7's own S-1 concerns ir_dump_verify invariant-12 test coverage — a different suggestion, not this hazard.)

Desired outcome: repo-backed sibling cards whose owned paths intersect must be structurally serialized or workspace-isolated at creation time, so that (a) one card's uncommitted edits cannot pollute another card's review/verification evidence in a shared working tree, and (b) the enforcement does not depend on reviewer discipline or on manual Coordinator diligence surviving normal operation.

This is a process-control design (Process Engineer domain). Adoption of any Operations Manual or profile change is a Main Designer / Marcel decision; this document supplies the analysis, the evidence, and the drafted proposal.

## 2. Evidence

All evidence was inspected read-only on 2026-08-12 from the shared kanban DB (`C:\Users\marce\AppData\Local\hermes\kanban.db`) and the AI-Co repository. Event times are local (+0200).

### 2.1 Primary incident (16b1 MIN-1 / MIN-2)

| Fact | Evidence | Source |
|---|---|---|
| Both cards share the same `dir` workspace path | `tasks.workspace_kind='dir'`, `tasks.workspace_path='E:\Hermes_Agent\projects\Sneedworks\projects\AI-Co'` for t_171e3b7f and t_30ae127a | kanban DB |
| Both cards edit the same owned files | t_171e3b7f commit edc8db7: `bootstrap/src/ir/ir_core.c` (+7), `bootstrap/src/ir/ir_core_test.c` (+124); t_30ae127a commit 51fd411: same two files (+16/-1, +148) | git show --stat; card handoffs |
| No structural serialization between the siblings | `task_links` shows only parent t_19837659 for both; no edge t_171e3b7f → t_30ae127a | kanban DB |
| MIN-1 worker run | run 770: 19:49:22 → 19:57:24 (blocked for review) | task_runs |
| MIN-2 worker run (overlaps MIN-1's review) | run 772: 19:58:23 → 20:21:47 (blocked for review) | task_runs |
| MIN-1 independent re-review window | t_9ac8e485 run 774: 20:10:25 → 20:19:45 | task_runs |
| MIN-2's uncommitted edits present in shared tree during MIN-1 review | reviewer2: "the MIN-2 specialist's in-progress uncommitted edits to `ir_core.c`/`ir_core_test.c` appeared in the shared working tree (mtime 20:16 local)"; reviewer2 verified from isolated clean worktree at edc8db7 | t_9ac8e485 comment 1279 (S-1) |
| MIN-1 specialist's own re-verification may have read a tree already touched by MIN-2 | MIN-1 "fresh scripted verification" comment posted 19:59; MIN-2 run started 19:58:23 — the shared tree was the same path; no evidence the specialist used an isolated checkout | t_171e3b7f comment 19:59; task_runs |

**Interpretation (measurement, not assertion):** MIN-2's worker run and MIN-1's independent review window overlapped by ~21 minutes (19:58:23–20:19:45) in the same working tree, while both cards owned the same two files. The reviewer's clean-worktree discipline prevented misattribution; the hazard is that the shared tree's state was not a trustworthy evidence surface during that window, and nothing in the automated system detected or prevented the overlap.

### 2.2 Live recurrence at time of analysis (strongest evidence)

While this analysis was being performed, the identical pattern re-occurred on the WP-M0-16b2 review (t_47cce3e7 MIN-1/MIN-2, review of t_4bf3940f):

| Fact | Evidence | Source |
|---|---|---|
| 16b2 MIN-1 (t_95984b78) and MIN-2 (t_be99e361) created in the same Coordinator batch | both `created_at` = 2026-08-12 20:13:01 | kanban DB |
| Both children of t_4bf3940f, no edge between them | `task_links`: parent t_4bf3940f for both | kanban DB |
| Both share the same `dir` workspace path | `workspace_kind='dir'`, same `workspace_path` | kanban DB |
| Both own the same files | bodies list `bootstrap/src/ir/ir_dump.c`, `ir_dump.h`, `ir_dump_test.c` (regex-extracted) | kanban DB |
| t_95984b78 RUNNING and t_50708e5d (this card) RUNNING concurrently in the same dir right now | task_runs: t_50708e5d started 22:50:08; t_95984b78 started 22:52:08; both `status='running'` with live heartbeats 22:55–22:56 | kanban DB |
| t_be99e361 READY awaiting dispatch into the same dir | `status='ready'`, same `workspace_path` | kanban DB |

**Interpretation:** At the time of writing, TWO cards with disjoint owned paths (this process card, owned `docs/planning/`; t_95984b78, owned `bootstrap/src/ir/ir_dump*`) are executing CONCURRENTLY in one shared working tree, and a THIRD sibling card (t_be99e361) is queued into the same tree. No file overlap exists in this specific instance (owned paths differ), so no collision is expected — but the shared tree is again not a trustworthy evidence surface for either card's verification, and the system has no automated mechanism preventing or flagging this state.

### 2.2a Direct first-hand observation during this analysis (strongest live evidence)

While this record was being written, the hazard was observed live in the very shared tree under analysis:

| Fact | Evidence | Source |
|---|---|---|
| t_95984b78 (16b2 MIN-1) running in the shared dir during this analysis | task_runs: started 22:52:08, live heartbeat 22:58:17 | kanban DB |
| Its in-progress uncommitted edit appeared in the shared tree mid-analysis | `bootstrap/src/ir/ir_dump_test.c` mtime = 2026-08-12 22:58:13 (+0200), inside t_95984b78's running window | filesystem stat (read-only) |
| Working tree of the shared dir went dirty with the other card's edit while this card ran | `git status --porcelain` at commit time showed `M bootstrap/src/ir/ir_dump_test.c` (190 insertions) alongside this card's untracked new file | git status |
| Three cards now share the same `dir` workspace concurrently | t_50708e5d (running 22:50:08), t_95984b78 (running 22:52:08), t_22a99eac coordinator TRIAGE (running 22:57:09) — all `workspace_kind='dir'` at `E:\Hermes_Agent\projects\Sneedworks\projects\AI-Co` | kanban DB task_runs |

**Interpretation:** This is the exact failure class S-1 describes, observed first-hand without relying on a prior reviewer's report: a second card's uncommitted source edit (`ir_dump_test.c`, mtime 22:58:13) appeared in the shared working tree while this card (t_50708e5d) was actively running and verifying its own evidence. This card's owned area is `docs/planning/`, so no misattribution occurred here — but the same state, with overlapping owned paths, is precisely the 16b1 MIN-1/MIN-2 incident. The system did not detect or flag the concurrent shared-dir state; no W2c probe exists.

### 2.3 Workspace-kind and probe coverage facts

| Fact | Evidence | Source |
|---|---|---|
| `worktree` workspace kind is never used | `SELECT workspace_kind, COUNT(*) FROM tasks GROUP BY workspace_kind` → dir 552, scratch 61, worktree 0 | kanban DB |
| No shared-dir concurrency probe exists in the watchdog | `grep -n "running" kanban_review_router.py` → no hits; `grep -n "workspace_kind" kanban_review_router.py` → only line 720 (W3 mtime scoping inside `progress_evidence`); no function detects two running cards sharing one dir path | governance/runtime/scripts/kanban_review_router.py |
| OM §6 W2 lists the probe as **optional** | W2 enforcement point (c): "optional watchdog probe: two `running` cards sharing one `dir` workspace path is escalated to the Coordinator" | OPERATIONS_MANUAL.md v1.1.9 line 182 |
| Coordinator profile contains W2 prose but no automated gate | COORDINATOR_PROFILE v1.0.2: "Before dispatching a second card into a shared `dir` workspace, run the non-destructive `git status --porcelain` cleanliness check... represent serialization with a structural parent or gate dependency edge" — a manual procedural check | COORDINATOR_PROFILE.md line 186 |
| Manifest requires serial sibling edges for split cards but the check is prose | "Each split card must have: ... serial edges to its siblings and the next package" and "no split card may depend on files owned by a later sibling"; "Dispatch sub-split siblings strictly serially"; "No parallel lanes within M0" | AI-CO-STAGE0-WORK-PACKAGE-MANIFEST-2026-08-09.md lines 54, 84, 1233 |

### 2.4 Governing sources consulted

- OPERATIONS_MANUAL.md v1.1.9, §6 Workspace integrity and provenance Rules W1–W4 (adopted v1.1.7, 2026-08-12, from Process Engineer proposal commit 1cf70ce) — the affected rule set.
- OPERATIONS_MANUAL.md §6.1 blocked-review routing and watchdog mechanism; §18 Process improvement.
- COORDINATOR_PROFILE.md v1.0.2 — pre-assignment verification (W1a, W2a) and W4 classification steps.
- AI-CO-STAGE0-WORK-PACKAGE-MANIFEST-2026-08-09.md — serial-discipline and splitting rules.
- Review verdict t_9ac8e485 (PASS, 1 Suggestion S-1 — workspace isolation of sibling tasks, comment 1279) — the sole S-1 source for this hazard. (t_47cce3e7 is PASS WITH MINOR FINDINGS, but its S-1 concerns ir_dump_verify invariant-12 test coverage — a different suggestion, not a source for this hazard.)

### 2.5 Alternative explanations considered

- **"The two 16b1 workers never executed simultaneously, so the hazard is theoretical."** Rejected as insufficient: the hazard is not only simultaneous worker execution; it is any overlap between one card's uncommitted working-tree state and another card's review/verification window. That overlap is empirically confirmed (review window 20:10:25–20:19:45 vs. MIN-2 run 19:58:23–20:21:47). Additionally, the live recurrence (t_50708e5d + t_95984b78 both `running` now) shows true concurrent execution in one dir across different profiles.
- **"Reviewer discipline (clean worktrees) already protects evidence."** Reviewer isolation worked here, but it is a manual compensating control, not an automated control; the org already classifies reliance on a reviewer's proactive isolation as an unacceptably weak enforcement basis (same reasoning as W1–W4 adoption).
- **"Same-profile dispatch serializes workers, so nothing can collide."** Both 16b1 cards were assigned to senior_specialist and happened to serialize; but review cards run under reviewer2 and this card runs under process_engineer, so cross-profile overlap is real and currently live. Assignee-profile serialization is not a structural guarantee.
- **"The manifest already forbids parallel lanes."** The manifest rule is prose at creation time; no structural edge was created for either the 16b1 or 16b2 sibling pair. Prose is exactly what W2's "structural board edge, not prose" clause was intended to eliminate.

## 3. Rule gap vs. enforcement gap — assessment

**Conclusion: primarily an enforcement gap, with a secondary rule-precision gap.** W1–W4 as adopted are necessary and largely sufficient as *principles*; the failure is that the designed enforcement did not exist (W2c probe never implemented) and card creation did not apply the existing serialization mandate to review-follow-up siblings.

Per rule:

- **W1 (scratch provisioning):** Not implicated. Both incidents used `dir` workspaces, not scratch. No change needed.
- **W2 (single-owner-per-dirty-tree):** Partially implicated — rule-precision gap + enforcement gap.
  - *Rule precision:* W2's dispatch-time gate is "the tree has no uncommitted changes." In both incidents the tree WAS clean at the second sibling's dispatch time (MIN-1 committed edc8db7 before MIN-2 started). The rule regulates the *state* (dirty tree) but not the *concurrency* (two cards dispatched into one clean tree whose owned paths intersect). W2's designed control for concurrency — enforcement point (c), the shared-dir probe — is explicitly marked **optional** and is **not implemented** in `kanban_review_router.py`.
  - *Enforcement:* the Coordinator's W2a check is a manual procedural step in the profile; it cannot be relied on to catch siblings that promote simultaneously after a parent's terminal completion, and it has no automated backstop.
- **W3 (watchdog progress evidence scoping):** Implemented and working (router line 720 mtime scoping). It mitigates evidence conflation for *continuation decisions*; it does not prevent the underlying shared-dir concurrency. No change needed, but W3's provenance discipline is the model for what the probe should do.
- **W4 (provenance-before-attribution):** Implemented as a required classification procedure. The reviewer's clean-worktree verification effectively applied W4. No change needed.

**Root cause:** card creation produces sibling remediation cards (one review's MIN-1/MIN-2) as parallel children of the reviewed task with overlapping owned paths, no serial edge between them, and the same `dir` workspace; the only designed automated control (W2c probe) was never built; the only remaining controls are manual (Coordinator diligence, reviewer isolation). Manual controls fail under normal operation (both recurrences prove it).

## 4. Proposed change (hypothesis and measures)

**Affected rule:** OM §6 W2 (and, by reference, the Coordinator profile's pre-assignment verification and the manifest's serial-sibling practice).

**Normative amendment (draft, for the Main Designer to execute and Marcel to approve):**

> **W2a (extension).** A repo-backed card whose owned paths intersect another active card's owned paths must, at creation time, be either (1) structurally serialized by a dependency edge to that card (the later card stays `todo` until the earlier is terminally done), or (2) provisioned with an isolated workspace (`workspace_kind: worktree` for git-backed repos, or a dedicated clone) instead of the shared `dir` workspace. The Coordinator verifies both conditions at card creation; a sibling pair whose owned paths intersect and that lacks a serial edge or workspace isolation is not dispatched.
>
> **W2c (promoted from optional to mandatory).** The watchdog detects any two cards in `running` status sharing the same `dir` `workspace_path` and escalates a deduplicated Coordinator triage card (read-only detection; mirror of the existing stale-review escalation pattern). The probe fires on the first observation; the Coordinator resolves by serializing (edge), isolating (worktree), or accepting a documented no-overlap rationale per W4.

**Hypothesis:** requiring a structural serialization edge or worktree isolation for repo-backed sibling cards with intersecting owned paths, backed by a mandatory shared-dir watchdog probe, reduces the covered hazard class (review-window pollution, cross-card evidence conflation, concurrent uncommitted edits to the same files in one tree) to zero observed occurrences within the trial period, without measurable dispatch delay beyond the intended serialization.

**Alternatives considered:**

- **A. No change (rely on W1–W4 + reviewer discipline).** Rejected: two recurrences in one evening demonstrate manual enforcement does not survive normal operation; the live concurrent `running` state is observable today.
- **B. Default all repo-backed AI-Co cards to `worktree` kind.** Strongest isolation but highest operational cost: changes build/verification paths for every card, requires re-provisioning conventions, and conflicts with the manifest's explicit `dir`-kind workspace convention for M0 cards (manifest rule 3). Adopted as the *fallback* within W2a (option 2), not as a blanket default.
- **C. Serialize by profile (trust same-assignee serialization).** Rejected: cross-profile overlap is live (this card + t_95984b78), and assignee assignment is not a structural guarantee.
- **D. Prose-only manifest reminder.** Rejected: the manifest already has the prose; prose failed twice.

**Trial scope (bounded):** applies to AI-Co repo-backed sibling cards created from review findings (MIN-1/MIN-2 pattern) for 30 days from adoption; watchdog probe implemented in `kanban_review_router.py` (versioned copy in `governance/runtime/scripts/`); no changes to dispatch authority, review class, gate rules, or completion rules.

**Owner:** Coordinator (card-creation verification and any resolution cards); Main Designer (OM §6 amendment text, profile amendment, watchdog probe implementation); Process Engineer (monitoring and evaluation at review date); Marcel (approval of the OM amendment).

**Safety limits:** the watchdog probe is read-only and only comments/creates a Coordinator escalation card (no unblock, no block, no completion, no dispatch change); no destructive action is authorized; the continuation cap and review gate rules are unchanged; no owned-path or acceptance-criteria changes; no C: drive writes; no secrets.

**Success measures:** (a) zero new observations of one card's uncommitted edits appearing in another card's review/verification window for repo-backed sibling cards; (b) zero shared-dir concurrent `running` pairs that lack a documented no-overlap rationale; (c) W2c probe fires and the Coordinator resolves within 24 hours on each observation; (d) no regression in review-pass rates or gate evidence quality attributable to this change.

**Adverse-effect measures:** (a) dispatch/blocked-time delay on serialized sibling cards beyond the intended ordering (track blocked-time delta vs. pre-trial baseline); (b) probe false positives (running pairs whose owned paths do not intersect and which were correctly dispatched); (c) unintended `worktree` provisioning breakage on cards that chose isolation; (d) any Coordinator escalation card that turns out to be a non-issue.

**Review date:** 2026-09-11, or on first recurrence of the covered hazard class, whichever comes first. Independent review need: Main Designer coherence review of the OM text; Reviewer conformance review of the watchdog code change per OM §11; Process Engineer preserves negative or inconclusive results.

**Rollback:** revert the OM §6 W2 amendment and version bump, revert the router probe change (versioned script copy), revert the profile amendment — all by the Main Designer through the same approval path; trial-period records preserved as evidence per OM §8. The W2a/W2c change is low-risk and reversible; adoption may proceed without a Constitution §8 temporary exception (not a constitutional change).

## 5. Implementation handoffs (routing via Coordinator, per OM §6 batch sizing)

| # | Change | Owner | Notes |
|---|---|---|---|
| I1 | OM §6 W2 amendment (W2a extension + W2c mandatory) with v1.1.10 change record | Main Designer (executes; Marcel approves adoption) | Draft text in §4 above |
| I2 | Watchdog probe in `kanban_review_router.py` (detect ≥2 running cards sharing one dir `workspace_path` → deduplicated Coordinator escalation) + unit test | Main Designer (watchdog mechanism owner) | Follow the existing stale-escalation and dry-run/comment pattern |
| I3 | COORDINATOR_PROFILE v1.0.3: card-creation verification for intersecting owned paths (serial edge or worktree isolation) | Main Designer (profile owner) | Extends v1.0.2 W2a prose |
| I4 | Immediate operational enforcement: for the currently-ready t_be99e361 (16b2 MIN-2) and any future review-follow-up siblings, add a serial edge or isolate workspace before dispatch | Coordinator | Live recurrence at time of writing |

No change is proposed to W1, W3, W4, review verdicts, acceptance criteria, the M0 manifest's package contents, or production code. The manifest's existing serial-sibling prose (line 84) is strengthened by enforcement, not rewritten.

## 6. Confidence and limitations

- Requirements confidence: High (explicit reviewer S-1 on t_9ac8e485 — workspace isolation of sibling tasks — routed by Coordinator; t_47cce3e7's S-1 is a different suggestion, invariant-12 test coverage, and is not a source for this proposal).
- Architecture confidence: High (process-control analysis; no product-architecture impact).
- Verification confidence: High for the recurrence facts (DB queries, run windows, router grep, file stats); Medium for the causal attribution "manual enforcement failed because..." — supported by two independent recurrences and the confirmed absence of any automated probe.
- Limitations: the live recurrence involves this analysis card itself (owned paths disjoint), so it is evidence of the *concurrency state* the probe should flag, not of file-collision damage; file-collision damage remains unobserved and is bounded as a risk, not asserted as an occurred harm.

## 7. Follow-up routing

- Main Designer: I1–I3 (coherence review and execution) per OM §18 / Constitution §8.
- Coordinator: I4 immediate enforcement; then creation of the I1–I3 cards per OM §6 batch sizing.
- Historian: index this proposal and the eventual OM v1.1.10 change record.
- Process Engineer: monitor measures at the 2026-09-11 review date; preserve negative results.
- Marcel: approval of the OM §6 amendment (adoption path), per the Constitution.
