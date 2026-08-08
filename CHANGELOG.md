# Changelog

All notable changes to this project are recorded here, following
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and
[Semantic Versioning](https://semver.org/).

---

## [Unreleased]

### Added
- `cmake/EthervoxFeatures.cmake` (TASK-C1.0): `ETHERVOX_FEATURE_HTTP`, `_DOWNLOADER`,
  `_BUG_REPORT`, `_WEATHER`, `_FILE_TOOLS` options, all defaulting `ON` (no behaviour change for
  existing consumers). Selecting any of them `OFF` now excludes the corresponding subsystem from
  the build:
  - `_WEATHER`: excludes `src/plugins/weather_tools/*` and its four runtime registration call
    sites.
  - `_BUG_REPORT`: excludes `src/common/bug_reporter.c` and the `/report` CLI command.
  - `_HTTP`: excludes `src/common/platform_http.c`; `model_downloader.c` falls back to its
    existing "manual download required" path.
  - `_DOWNLOADER`: `ethervox_model_download()` returns the new `ETHERVOX_ERROR_FEATURE_DISABLED`
    before reaching the network path; `src/llm/model_manager.c`'s independent
    `USE_LIBCURL`/`USE_WININET` download path (previously ungated) is composed with this flag too.
  - `_FILE_TOOLS`: excludes `main.c`'s file-tools init/register block.
- `scripts/check-no-network-symbols.sh` (TASK-C1.0): scans a static library's undefined symbols
  for `socket`/`connect`/`getaddrinfo`/`gethostbyname`/`curl_*`/`CFNetwork`/`NWConnection`/
  `CFSocket`, failing if any survive. Wired into CI (`workspace-network-symbol-check` job in
  `.github/workflows/build-and-test.yml`), which configures with all five feature flags `OFF`.
- `ETHERVOX_ERROR_FEATURE_DISABLED` and `ETHERVOX_ERROR_NO_TTS_HOST` error codes.
- `ethervox_tts_host_t` (TASK-C1.0, `docs/07-BACKEND-CHANGES.md` §0.4): the TTS-as-a-host-capability
  interface (`include/ethervox/tts_host.h`, `src/tts/tts_host.c`) —
  `speak`/`stop`/`pause`/`resume`/`is_speaking`, `ethervox_tts_style_t`, `ethervox_tts_word_cb`,
  `ethervox_tts_set_host`. A missing host returns `ETHERVOX_ERROR_NO_TTS_HOST`, never crashes.
  `tests/unit/test_tts_host.c` verifies speak/stop/pause/resume/is_speaking/word-callback against
  a fake host. Not yet wired into `dialogue_core.c`/the conversation state machine, and Piper/the
  phonemiser are not yet deleted — both pending a follow-up session (barge-in needs verifying end
  to end before the old path can go).

### Changed
- All CC BY-NC-SA 4.0 licence references in `src/` and `include/` (132 files: SPDX identifiers,
  "Licensed under…" lines) rewritten to reference the proprietary licence, authorized by
  `LICENSE`'s "Copyright Holder Reservation of Rights" clause.

### Removed
- `src/tmp.txt` (tracked debug-log dump); `main.c.bak`/`tmp.txt` added to `.gitignore`.
