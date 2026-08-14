# AI-Co Repository License — GPL-3.0

**Status:** Accepted (Human Sponsor approval, 2026-08-14)
**Author:** Main Designer (recorded 2026-08-14)
**Applies to:** AI-Co repository (public, https://github.com/qazW12345/AI-Co.git)

## Decision

The AI-Co repository is licensed under the **GNU General Public License version 3 (GPL-3.0)**.

- Human Sponsor (Marcel) directed the change on 2026-08-14: "I would like to use the GPL 3.0 license."
- Scope confirmed by Marcel on 2026-08-14: the license applies to the **AI-Co repository** (the public one). It does not extend to the Sneedworks organization root repository or its governance documents, which remain unlicensed/internal.
- Replaces the current placeholder `LICENSE` file ("NO LICENSE GRANTED ... until Marcel approves a license").
- Copyright holder line: `Copyright (C) 2026 qazW12345` (GitHub handle per Marcel's direction 2026-08-14; handle preferred over a personal legal name for now).

## Context

- The repository was created with an explicit "no license granted" placeholder pending Human Sponsor approval. This record is that approval.
- The project charter prioritizes self-sufficiency, minimal external dependence, and long-term local operation. GPL-3.0 is consistent with those priorities: it grants the four freedoms to all downstream users while requiring derivative works of the compiler itself to remain GPL-3.0.
- AI-Co programs compiled by the AI-Co compiler are **not** covered by the compiler's license (GPL does not extend to compiler output); user programs are the user's own copyright. The license covers the compiler implementation, specifications, tests, and documentation in this repository.

## Consequences

Positive:
- Grants clear permission to use, study, modify, and redistribute the compiler and its ecosystem under GPL-3.0 terms — removing the ambiguity of the placeholder.
- Aligns with the project's public, open posture and agent-oriented design (agents may read, modify, and redistribute under the license terms).
- GPL-3.0 includes patent and anti-Tivoization protections relevant to a self-hosting toolchain.

Negative / costs:
- Strong copyleft: any distributed derivative of the compiler implementation must be GPL-3.0; this may discourage some commercial/proprietary reuse. Accepted by the Human Sponsor.
- Per-file SPDX headers are recommended best practice but **deferred**: adding headers to all source files (367 `.c`/`.h`/`.py` files today) is a large mechanical change with review cost; a top-level `LICENSE` file satisfies GPL-3.0 conveyance requirements. A follow-up may add standardized headers in a single sweep.

## Implementation

- Replace `LICENSE` with the canonical GPL-3.0 text (official FSF copy, e.g. https://www.gnu.org/licenses/gpl-3.0.txt) plus the copyright line above.
- Update `README.md`: License section (GPL-3.0, link to LICENSE), Status section (M0 implementation in progress — currently WP-M0-17 backend), spec links (Accepted), and general repository-layout/building pointers.
- Commit locally; push only via the review-gated push wave per the push rule.
