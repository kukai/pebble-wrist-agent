# Changelog

- 2026-07-19: onboard executed — 1 file migrated (CLAUDE.md → CLAUDE.legacy.md, summarized into Project-Specific); decisions location = `docs/adr/ADR.md`
- 2026-07-29: added two Hard Rules — confirm concrete before/after behavior for ambiguous verbs before coding, and check `git branch --show-current` before every commit in multi-branch sessions. Reason: the timer/stopwatch "reset" feature was redefined 5+ times (ADR-027–034) after each implementation guessed at the meaning, and two commits landed on `staging` instead of the feature branch.

