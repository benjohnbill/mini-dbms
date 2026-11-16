# Issue tracker: Local Markdown

PRDs and tickets for this repo live as Markdown files in `docs/prd/`. They do not live in GitHub Issues. There are no milestones: one final PRD covers LMS Phase 1 to Phase 3.

## Conventions

- A PRD draft is `docs/prd/<initials>.md` (for example `docs/prd/CJG.md`). Each member writes one after a grill session with AI. It stays on that member's personal branch until the team meeting.
- The final PRD is `docs/prd/final.md`. The team writes it after the meeting compares the drafts.
- Tickets are one file each: `docs/prd/issues/<NN>-<slug>.md`, numbered from `01`. Never put all tickets in one file.
- The triage state is a `Status:` line near the top of each file. See `triage-labels.md`.
- Comments and discussion go at the bottom of the file under a `## Comments` heading.

## PRD drafts are history

- A PRD draft records what one member thought before the meeting. Do not edit a draft after the final PRD exists.
- The current state of a feature is in `src/`, `tests/`, and `README.md`.

## When a skill says "publish to the issue tracker"

For a draft, write `docs/prd/<initials>.md`. For the final PRD, write `docs/prd/final.md`. For a ticket, create a file under `docs/prd/issues/`.

## When a skill says "fetch the relevant ticket"

Read the file at the given path. The user normally gives the path or the ticket number.
