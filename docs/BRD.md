# KEEBY — Business Requirements Document

Status snapshot date: 2026-09-26. This is a personal, non-commercial project; "business" here means the decision context around it, not a market or a company.

## 1. Business context

KEEBY is a Linux clone of Keeby (getkeeby.com), built for one person's own use on their own CachyOS + Niri machine. There is no revenue, no customers, no distribution plan beyond that one machine. The "business" is: does the owner get a tool they actually use daily, at a quality and feel close to the paid macOS/Windows product they can't run on Linux. [inferred: the trigger was that Keeby has no Linux build and the user wanted the same experience.]

## 2. Objectives and success metrics

| Objective | Metric | Notes |
|---|---|---|
| Replace Keeby as a daily-driver tool | Owner uses KEEBY during normal typing sessions without turning it off due to bugs/annoyance | Subjective; no telemetry exists or is planned |
| Feature parity with Keeby's advertised feature set | Parity checklist: fraction of PRD FR-01..FR-21 at "Done & live-verified" | Currently 11/21 live-verified, 7 done-tests-only, 3 in progress, 3 not started (see traceability table) — [inferred: "not started" excludes explicit non-goals like asset copying] |
| No regression to normal keyboard/mouse use | Zero missed/duplicated keystrokes; zero PipeWire xruns during typing | Live-verified for the core engine per HANDOFF §4 |
| Visual/behavioral fidelity to the reference recording | Owner's side-by-side judgment against `~/Videos/recording_2026-09-25_22.12.31.mp4` | Only method available; there is no automated visual-diff process |
| Safe to run continuously | No unexplained crash of `keeby.service`; security posture holds (no elevated group membership leaking into the running processes) | Checked at each package release via the live security re-check in HANDOFF §6c |

No market-size, revenue, or competitor-adoption data exists or is claimed anywhere in this document; anything that reads like a business fact is either sourced from `HANDOFF.md`/`docs/` or marked **[inferred]**.

## 3. Stakeholders

| Stakeholder | Role |
|---|---|
| Project owner (the user) | Sole owner, sole decider, sole end user. Approves all scope changes, especially privilege/permission changes (e.g. widening `keeby-inputd`'s device access). Only person who can authorize a commit, an install, or a security-relevant merge. |
| AI orchestrator (top-level agent) | Plans and verifies only; does not implement directly. Spawns and reviews worker subagents. |
| AI worker subagents | Implement scoped, self-contained tasks (a feature, a patch) under the orchestrator's brief and acceptance criteria. |

There are no other stakeholders — no team, no external users, no support obligation.

## 4. Requirements traceability

| Business objective | Traces to PRD requirement(s) |
|---|---|
| Daily-driver sound experience without breaking typing | FR-01, FR-02, NFR-01, NFR-02 |
| Feature parity: sound character (pitch, pan, tone, packs) | FR-03, FR-04, FR-05, FR-06, FR-07 |
| Feature parity: control surfaces | FR-10, FR-11, FR-12, FR-13 |
| Feature parity: visualizer | FR-14, FR-15, FR-16, FR-17, FR-18 |
| Feature parity: not-yet-built extras | FR-08, FR-09 |
| Keystroke privacy / safe to run continuously | NFR-03, NFR-06, NFR-08, FR-19 |
| Real-time audio stability | NFR-02, NFR-04 |
| Runs correctly under this Wayland compositor | NFR-05, §6 platform constraints |
| Installable and repeatable on this machine | FR-20, FR-21 |
| Verification discipline before calling anything "done" | NFR-07 |

## 5. Constraints

- **Licensing of sound packs**: Mechvibes community packs carry no consistent or verified license; they are never bundled into the KEEBY package and are always fetched by the user themselves (`keeby-fetch-packs`) into `~/.local/share/keeby/packs`. KEEBY's own code only reads a format it doesn't own or redistribute content for.
- **No Keeby assets**: Keeby's recordings, logo, and artwork must not be copied into KEEBY. Only the *behavior* (sound design pattern, UI layout/motion language) is replicated, from the owner's own observation of the product.
- **Security posture**: keystrokes are treated as confidential. Only one privilege-separated helper process (`keeby-inputd`) ever opens `/dev/input`; it drops privilege immediately after opening the device and runs with zero Linux capabilities and `NoNewPrivs=1`. No raw keystroke ever crosses D-Bus or a named socket. Any scope widening (e.g. reading pointer devices for the visualizer) requires the owner's explicit prior approval — this happened once already for pointer-motion estimation.
- **Single-machine target**: CachyOS + Niri + this hardware only. No packaging or testing effort is spent on portability.
- **No project license chosen yet**: `packaging/arch/PKGBUILD` ships `license=('custom')` as a placeholder pending an owner decision (see PRD open questions).
- **Verification discipline**: nothing user-facing is accepted as done from unit tests alone; the owner must see/hear it live. This slows down "percent complete" claims but is a hard constraint set by the owner (see `~/.claude/.../feedback_keeby_step_discipline.md`).

## 6. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Keystroke capture is structurally keylogger-equivalent capability | Privilege-separated helper with immediate privilege drop, zero capabilities, no persistence/logging of key sequences, small auditable capture surface (NFR-03) |
| Wayland has no absolute cursor position, so cursor-follow is inherently approximate | Relative pointer-motion estimation with edge resync and a user-tunable Follow Speed; owner explicitly accepted "estimate" as the tradeoff rather than blocking the feature |
| Careless changes to the PipeWire RT callback cause audible glitches (xruns) | Mandatory Release + ASAN/UBSAN + TSAN verification gate on every merge touching the audio path (NFR-02) |
| Bundling or copying third-party/Keeby assets creates legal exposure even for a personal project | Hard rule: packs are user-fetched, never bundled; no Keeby recordings/logo/artwork copied at all |
| Two large in-flight patches (pointer motion, visualizer overlay) touch the same shared files and risk a bad merge | Documented merge order (pointer patch first, reconcile overlay against it) and a single supervisor review pass before either is applied (HANDOFF §6b) |
| Sole-owner/sole-operator project has no continuity if context is lost between sessions | `HANDOFF.md` and the numbered `docs/00N-*.md` step reports are kept current specifically so a new AI agent session can resume without the owner re-explaining state — [inferred: this is the documents' evident purpose, not a stated policy] |
| "Done" claims drift ahead of what's actually verified | Owner-enforced rule: unit tests alone never justify a "done" claim on user-facing behavior (NFR-07); this BRD and the PRD status column reflect that split explicitly |

## 7. Assumptions

- The owner remains the sole user and sole decision-maker for the foreseeable life of this project. [inferred]
- The CachyOS + Niri + PipeWire + sdbus-c++ + GTK4 toolchain on the owner's machine stays compatible across package updates; no cross-distro or cross-compositor compatibility is being tracked.
- The Mechvibes pack ecosystem remains freely downloadable by the owner; KEEBY does not need to host or mirror packs.
- There is currently no plan to open-source, publish, or otherwise distribute KEEBY beyond the owner's machine. [inferred — no statement either way exists in HANDOFF.md or the docs read for this document]

## 8. Out of scope

- **Distribution**: no AUR submission, no public repository hosting, no release process for other users. [inferred: nothing in the source docs proposes this]
- **Monetization**: none; KEEBY is not sold, and Keeby's $4.99 price point is background context only, not a target KEEBY has to hit or beat.
- **Multi-user or system-wide install**: packaging targets one user's session (`systemd --user`), not a shared/system-wide deployment.
- **Support/maintenance commitments**: there is no SLA, no bug tracker for external users, no roadmap beyond the owner's own backlog (HANDOFF §7).
- **Cross-platform builds** (Windows/macOS/other Linux desktop environments) — explicitly out of scope per PRD §7.
