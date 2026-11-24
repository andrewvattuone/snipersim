# Stage 1–7 handoff material

This directory holds documentation only, written at the stopping point after Stage 7 (2026-11-23). Nothing here
changes code, configuration or results. It is on branch `docs/stage1-7-handoff`, branched from `feature/rl-port`
2363b52e.

| File | What it is |
|---|---|
| [`EXECUTIVE_SUMMARY.md`](EXECUTIVE_SUMMARY.md) | The 1–2 page summary: goal, what was built, results, why the learner loses, the offline model, next step |
| [`HANDOFF_REPORT.md`](HANDOFF_REPORT.md) | The full report, sections A–G: objective, implementation, Stages 1–5, Stage 6, Stage 7, status, proposed Stage 8 |
| [`ADVISOR_PREP.md`](ADVISOR_PREP.md) | An 8-slide outline, a glossary, ten likely questions with answers, and points to state carefully |
| [`CODE_TOUR.md`](CODE_TOUR.md) | The code sections to read personally, with line numbers and the tests that exercise them |

**Conventions:**
* Results are labelled **measured**, **code fact**, **estimate** or **hypothesis**.
* Performance comparisons are two-repetition ranges unless stated otherwise.
* Nothing in this material claims that the RL algorithm outperforms the fixed priority. It does not.

## Suggested study order

About 6–8 hours in total. Each step builds on the previous one.

1. **The big picture** (30 min): `EXECUTIVE_SUMMARY.md`, then `HANDOFF_REPORT.md` sections A and "Where things
   are".
2. **MESI in Sniper** (1 h):
   * `HANDOFF_REPORT.md` B.2;
   * then `experiments/sharer-counts/COHERENCE_VALIDATION.md`, "Original failure" and "The fix";
   * then `CODE_TOUR.md` §4 in `cache_cntlr.cc` (the fill fix, the sharer hint, upgrades).

   Make sure you can explain why only M is writable, what an upgrade miss is, and why "M" can mean M-on-read.
3. **The algorithm** (1.5 h):
   * `HANDOFF_REPORT.md` B.3;
   * then `docs/rl/ALGORITHM_AUDIT.md` §3 (including the §3.2 worked example) and §5 issues I-1, I-2, I-4 and I-6;
   * then `CODE_TOUR.md` §1–3, reading `rl_reward_policy.h` end to end;
   * work the `POLICY_CORES.md` "Worked scenario" by hand and run `make -C testing/unit/rl test`.
4. **Validation** (30 min): `HANDOFF_REPORT.md` C, then the verdict and "Learning-event correctness" table of
   `docs/rl/STAGE5_VALIDATION.md`.
5. **Results** (1 h):
   * `HANDOFF_REPORT.md` D;
   * then in `docs/rl/CAPBENCH.md`, the "Ocean `small` no-learning comparison" ("Means", "What the comparison shows",
     "Ocean compared with LU `tiny`");
   * then `docs/rl/STAGE6_PILOTS.md` Pilot 3 ("What the comparisons show").
   * Be able to explain D.4 (why run time did not move) without notes.
6. **Why learning fails** (1.5 h):
   * `HANDOFF_REPORT.md` E.1–E.5;
   * then `docs/rl/STAGE7.md` §C (weight stability) and the Ocean pilot "Findings";
   * then `results/stage7-objective-investigation/REPORT.md` §§1–2 (E1–E6).
   * This is the core argument for Stage 8.
7. **The offline model** (1 h):
   * `HANDOFF_REPORT.md` E.6–E.8;
   * then `docs/rl/HEADROOM.md` on `review/stage7-headroom-trace`: §1 (the three questions), §2.4 (limits), §7.6
     (the first calibration), §7.7 (the investigation: "How Sniper decides whether a store reaches the L2") and §7.9
     (held-out).
8. **Status and next steps** (30 min): `HANDOFF_REPORT.md` F and G, then `ADVISOR_PREP.md`. Rehearse the ten
   questions.

## Before continuing development

* **Branches:** keep `review/stage7-protect-invalidation` and `review/stage7-headroom-trace` unmerged unless a review
  decides otherwise.
* **Results:** every result directory under `results/` is read-only. Verify archives from `results/` with
  `sha256sum -c archive/<name>.tar.gz.sha256`.
* **Stage 8:** it has not started. `HANDOFF_REPORT.md` G lists the directions and the recommended one to discuss.
