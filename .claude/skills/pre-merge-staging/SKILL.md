---
name: pre-merge-staging
description: >
  Pre-merge CloudPebble build check for this repo (pebble-wrist-agent). Use this
  whenever a reviewed PR or feature branch is about to be merged into `main` —
  before running the merge, route the branch through the `staging` branch first so
  CloudPebble builds it and the result can be confirmed good. Trigger this any time
  the user says things like "mainにマージして" / "merge this into main" / "ready to
  merge" / "let's ship this" for a change in this repo, or asks to verify a build
  before merging. Also use it to explain why a `staging` branch exists in this repo
  or how CloudPebble's build trigger relates to it.
---

# Pre-merge staging build check

## Why this exists

CloudPebble (the external build service for this Pebble app) is configured to
trigger a build on pushes to specific branches — `main`, and now `staging`
alongside it. There is no GitHub Actions workflow in this repo that builds or
tests the watch app (only a GitHub Pages deployment for `config/index.html`,
which fires on pushes to `gh-pages` and is unrelated). CloudPebble's own branch
settings are the only place that knows the app actually compiles.

`main` is treated as always-buildable. Merging straight into `main` means the
first proof the code compiles is on the branch nobody should ever see fail.
`staging` exists purely so that a CloudPebble build can be triggered and
checked *before* that merge, on a plain branch cut from `main` — no special
CI config lives in this repo for it.

## When to use this

Any time a branch/PR for this repo is judged ready and about to be merged into
`main`. Do this whether or not the user explicitly asks for a staging check —
it's the standing rule for this repo, not a one-off request.

## Steps

1. **Check the working tree is clean** before switching branches
   (`git status --short`). Stash or commit anything in progress first.

2. **Fetch and merge the target branch into `staging`:**
   ```bash
   git fetch origin <feature-branch> staging
   git checkout staging
   git merge --no-edit origin/<feature-branch>
   ```
   A fast-forward is expected and preferred (it means `staging` had no drift
   from `main`). If it's not a fast-forward, `staging` has diverged — see
   "Keeping staging from drifting" below before merging.

3. **Push `staging`:**
   ```bash
   git push origin staging
   ```
   This is the push CloudPebble is watching. No further action in this repo
   triggers the build — CloudPebble picks it up on its own.

4. **Check the CloudPebble build result** before proceeding. This tool doesn't
   have API access to CloudPebble, so ask the user to confirm the build
   succeeded (or point them to check it) rather than assuming it passed just
   because the push succeeded.

5. **Once the build is confirmed good, merge into `main` as normal** (via the
   PR, or a direct merge if that's the repo's usual flow for this change).

## Keeping `staging` from drifting

`staging` should track `main` plus whatever's currently being verified —
nothing more. After the real merge into `main` lands:

```bash
git fetch origin main
git checkout staging
git merge --ff-only origin/main
git push origin staging
```

If `--ff-only` fails, `staging` has accumulated commits that never made it
into `main` (e.g. an abandoned verification). Re-cutting `staging` fresh from
`main` is simpler than untangling that:

```bash
git fetch origin main
git checkout -B staging origin/main
git push origin staging --force-with-lease
```

Only force-push `staging` (never `main`), and only after confirming with the
user that nothing on `staging` is still needed — it's meant to be disposable
verification state, but check before discarding anything.
