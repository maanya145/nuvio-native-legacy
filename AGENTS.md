# Agent guide: maanya145/nuvio-native-legacy

This is a fork of [iqui27/nuvio-native-legacy](https://github.com/iqui27/nuvio-native-legacy),
a native C client (SDL2 + GLES2) for LG webOS, Samsung Tizen and Android TV.
This fork focuses on **large UHD Blu-ray remux playback on an LG B3 (webOS 23,
firmware 33.31.75)**: read-ahead buffering, original PGS subtitles, and DTS.

`CLAUDE.md` imports this file. Keep the two in sync by editing only this one.

## Hard rules

1. **Never install, launch, close or roll back anything on the TV without the
   user's explicit approval in the current conversation.** Reading logs and
   state over SSH is fine. Ask before any command that changes the TV.
2. **Never commit secrets.** `local.properties` (server URL, anon key, login
   base) is git-ignored and must stay that way. `tools/arm.sh` deletes any
   `.ipk` that contains a credential file. Never print stream URLs, debrid
   tokens or the anon key in output, logs or commits.
3. **Don't claim device behavior you didn't observe.** Host tests prove host
   behavior. Say "not tested on the TV" when that is the case.
4. Upstream contributions go out as small PRs from branches based on
   `origin/master` (see "Git"). Ask before opening, updating or closing any PR,
   on either repository.

## Repository map

| Path | What it is |
|---|---|
| `src/` | All C sources. Identifiers and comments are mostly Portuguese; keep each file's style. |
| `src/video.c` | webOS player glue: luna `com.webos.media` load/subscribe, track lists, DV/DTS routing. |
| `src/moviebuffer.c` | Loopback read-ahead proxy in front of the TV's URI player (Buffer size = 64/128/256 MB). |
| `src/mkvass.c` | Side reader for embedded MKV subtitles (ASS, text, PGS): index, Range reads, cluster scan. |
| `src/pgs.c`, `src/imgsub*.c` | PGS display-set decoder, cue store, GL overlay drawing. |
| `src/dts/` | FFmpeg engine, Starfish BUFFERSTREAM adapters, DTS conversion and Dolby Vision path. |
| `tests/` | ~1,200 host tests. Each `tests/<name>.sh` compiles a piece of `src/` with fakes and runs it. |
| `tools/` | Build, package and release scripts (`arm.sh`, `tpk.sh`, `build-dts-ffmpeg.sh`, ...). |
| `deploy/app/` | Files packaged into the webOS `.ipk` (art, fonts, `appinfo.json`). |
| `.claude/skills/` | Upstream release skills for Android and Samsung (Portuguese). |
| `tools/pgs-zlib-binpatch/` | Stopgap patch for the 2.0.8 release binary. Superseded by source builds. |

The root `.gitignore` ignores `/*.md` and `/docs/*.md` as internal notes. Only
`README.md`, `INSTALL*.md`, `AVISOS.md`, `AGENTS.md` and `CLAUDE.md` are
tracked at the root. Add an exception before committing a new root document.

## Setup

Run `bash tools/dev-setup.sh` first. It only checks and reports; it installs
nothing. It covers:

- **Docker** (OrbStack on macOS: `orb start`). Builds compile inside the
  `nuvio-webos-sdk` image (`tools/Dockerfile`, linux/arm64, about 4 GB), which
  holds LG's cross-compiler plus static ARM libass and FFmpeg. Build it once:
  `docker build --platform linux/arm64 -t nuvio-webos-sdk tools/`
  Rebuild it after changing `tools/build-dts-ffmpeg.sh` or `tools/build-ass-arm.sh`.
- **Host test tools** on macOS: `brew install ffmpeg openssl@3 ripgrep sdl2`
  (plus `coreutils` if you want `tools/testa-tudo.sh`, which calls `timeout`).
- **`local.properties`** for real builds. Without it `tools/env.sh` refuses to
  build a package. Ask the user for it; do not invent or extract values.

## Build the webOS package

```sh
NUVIO_CTR=docker NUVIO_RUST_MKVCORE=0 \
NUVIO_ARES_PACKAGE=/path/to/ares-package \
NUVIO_BUILD_JOBS=8 bash tools/arm.sh --ipk --build
```

- `--build` with `--ipk` only produces `space.nuvio.native.legacy_<ver>_arm.ipk`
  in the repo root. Without `--build`, `arm.sh` deploys to a TV: don't.
- `NUVIO_CTR=docker` is required on macOS; `tools/ctr.sh` otherwise defaults
  to Apple's `container` CLI.
- `NUVIO_RUST_MKVCORE=0` uses the C MKV parser, which is what the host tests
  exercise. Releases use the Rust core (`rust/mkvcore`); it needs the
  `armv7-unknown-linux-gnueabi` Rust target on the host.
- `--high-cache` raises the artwork texture budget. Nuvio caps it by RAM:
  160 MB on the 1.7 GB B3 instead of the normal 128 MB.

## Tests

```sh
bash tools/test-host.sh            # quick: PGS, read-ahead, DTS helpers
bash tools/test-host.sh mkvass     # every mkvass test (slow, a few minutes)
bash tools/test-host.sh pgs buffer # several groups
bash tools/test-host.sh tests/pgs.sh tests/dts_tv.sh   # explicit scripts
```

`tools/test-host.sh` gives each script its own timeout and an empty stdin, and
prints a pass/fail summary with log paths. Group names are listed by
`tools/test-host.sh --list`. Run the groups that cover what you touched:

| You changed | Run |
|---|---|
| `src/moviebuffer.c`, `src/dts/dts_engine.c` | `buffer` |
| `src/mkvass.c`, `src/pgs.c`, `src/imgsub*.c` | `pgs`, then `mkvass` |
| settings, translations (`src/ajustes.c`, `src/idioma_*.h`) | `tests/i18n.sh`, `tests/ajustes_ux_dados.sh` (needs SDL headers) |

Known timing-sensitive checks: `tests/moviebuffer.py` asserts startup under
0.4 s and `tests/mkvass_alem_teto.sh` measures pacing in real time. If one of
them fails right after Docker starts or under heavy load, rerun it alone before
treating it as a regression.

CI (`.github/workflows/fork-host-tests.yml`) runs the `quick` and `mkvass`
groups on Linux for every push and PR to this fork. Linux needs explicit
`-lm` and `-lz` where macOS does not, so link them in every test that
compiles `src/mkvass.c` or links the static host FFmpeg. Four upstream `mkvass`
tests only build on macOS (`mkv_legendas`, `mkvass`, `mkvass_url`,
`mkvass_texto`); CI skips them with `TEST_SKIP`, so run the `mkvass` group on
a Mac before merging changes to `src/mkvass.c`.

## The TV

- Device profile `tv` for the webOS CLI (`ares-novacom -d tv -r '<cmd>'`).
  Developer Mode SSH can refuse briefly after the TV wakes; retry once.
- App folder: `/media/developer/apps/usr/palm/applications/space.nuvio.native.legacy`.
  Since a reboot it is root-owned `775`, so you cannot create or rename files
  there; `nuvio-proto` itself is still writable. Stage files in
  `/media/developer/temp`, check `sha256sum`, keep a rollback copy, and only
  then overwrite in place. Get approval first (rule 1).
- The app's data folder `.nuvio/` lives inside the app folder. A reinstall
  with `ares-install` may wipe the user's login.
- Logs: `/tmp/nuvio.log` (masks URLs). Useful tags: `[movie-buffer]`,
  `[buffer]`, `[mkvass]` (including `buffer compartilhado`), `[dts]`, `[dv]`,
  `[tex] orcamento`, `buffering INICIO/FIM`, `[relogio]`.
- In `.nuvio/ajustes.txt`, toggles store **0 = on, 1 = off**.
- Don't let Nuvio's in-app update run on a test build; it replaces it.

## Git

- Remotes: `origin` = upstream `iqui27/nuvio-native-legacy`, `fork` =
  `maanya145/nuvio-native-legacy` (this fork).
- `fork/master` = upstream `master` plus this fork's work. Merge topic branches
  into it through PRs on the fork. Sync upstream with a merge, not a rebase:
  `git fetch origin && git merge origin/master` on a branch, then a PR to
  `fork/master`.
- Upstream PRs come from branches cut from `origin/master` that contain only
  that change. PR #763 (PGS zlib) is `fork/pgs-zlib-pgs`. Once upstream merges
  it, resolve the resulting conflicts in favor of upstream.
- `archive/*` branches keep superseded work for reference. Don't merge them.
- Commit messages: `area: summary` (for example `mkvass: ...`,
  `moviebuffer: ...`), body explaining why, in English.
- Worktrees share one stash. Never use bare `git stash`/`git stash pop`;
  commit WIP to a branch instead.

## Feature status (Nov 2026)

| Feature | Where | Status |
|---|---|---|
| Read-ahead starts without blocking (addon redirects) | `src/moviebuffer.c` | B3: stalls gone in testing |
| PGS zlib and header-stripped tracks | `src/mkvass.c` | B3: verified on a zlib track |
| Low-headroom PGS scan allowance | `src/mkvass.c` | Host tests only |
| Shared media buffer: subtitle Range reads copied from the read-ahead ring | `src/moviebuffer.c`, `src/mkvass.c`, `src/dts/dts_engine.c` | Host tests only, not built for or run on the TV |
| "Decoded by the TV" DTS option | not merged | Lives in a local checkout only |
| Incremental subtitle parser on the buffer stream (`src/subtap.c`) | `archive/shared-buffer-subtap-snapshot` | Dropped design, kept for reference |
