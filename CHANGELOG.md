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
- **`ETHERVOX_PROFILE`** (TASK-C1.2): Named feature profiles for target platform classes —
  `EDGE|MOBILE|DESKTOP|WORKSPACE`. Setting a profile automatically configures all feature flags:
  - `EDGE` (ESP32): All network/tool features OFF, no llama.cpp (~200KB target)
  - `MOBILE` (iOS/Android/RPi): All features ON (voice line with network)
  - `DESKTOP` (Friend'O'Mine): Same as MOBILE (pool/vision/embeddings future work)
  - `WORKSPACE`: DESKTOP minus all network features (HTTP, downloader, bug reporter, weather,
    built-in file tools) — enforces zero-network guarantee for Workspace shells
  Profile generates `ethervox_features.h` with `ETHERVOX_HAS_*` macros and `ETHERVOX_PROFILE_*`
  definitions. Conformance test (`tests/unit/test_profile_conformance.c`) verifies feature
  composition per profile.
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
  a fake host. **UPDATED**: Now wired into `voice_conversation.c`'s `conversation_on_speak()` —
  mobile/platform-native TTS path unchanged; desktop Piper path replaced with calls to
  `ethervox_tts_host_speak()` and `ethervox_tts_host_is_speaking()` for barge-in detection.
  Phonemiser/Piper deletion in progress (removed from file system and most CMake references; some
  consumers like `settings_menu.c` still need updating).

### Fixed
- `tests/unit/test_tts_host.c` and `tests/unit/test_voice_conversation.c`: replaced `assert()`
  with an explicit `CHECK()` macro — this test suite builds with `-DNDEBUG` (Release), which
  turns `assert()` into a silent no-op, so every check in these two files (and likely the rest of
  `tests/unit/`, not yet fixed — see BACKLOG-15) was passing vacuously regardless of correctness.
  Fixing this in `test_voice_conversation.c` (the barge-in test coverage) surfaced two real,
  previously-masked issues: a test-authoring bug in the grace-period exact-boundary case (the
  detector itself was correct — `ethervox_barge_in_detector_process`'s documented `>=` semantics),
  and a gap where `ethervox_conversation_init`/`start`'s background thread never reaches
  `ETHERVOX_CONV_STATE_ERROR` when local STT/TTS model files are absent (stays at
  `UNINITIALIZED` indefinitely instead) — worked around in the test (`SKIP` instead of fail), the
  underlying `voice_conversation.c` gap itself is unfixed (BACKLOG-15).

### Changed
- All CC BY-NC-SA 4.0 licence references in `src/` and `include/` (132 files: SPDX identifiers,
  "Licensed under…" lines) rewritten to reference the proprietary licence, authorized by
  `LICENSE`'s "Copyright Holder Reservation of Rights" clause.
- **Piper TTS backend and phonemizer** (TASK-C1.0): Removed `src/tts/phonemizer/` directory (25 files),
  `src/tts/piper_backend.c`, and `src/tts/tts.c` (old TTS API). TTS is now handled via the
  `ethervox_tts_host_t` platform callback interface. ONNX Runtime and espeak-ng dependencies removed.
  CMakeLists.txt cleaned: removed phonemizer source references, ONNX/Piper linking, espeak dictionary
  embedding logic (5 variant checks + `ENABLE_ESPEAK_DICT` option), MSVC `/bigobj` workaround.
  Platforms must now register a TTS host via `ethervox_tts_set_host()` before speaking.
  - `voice_conversation.c`: Fully rewired to use `ethervox_tts_host_speak()` and
    `ethervox_tts_host_is_speaking()`; removed Piper initialization; barge-in detection now polls
    host interface. `ethervox_conversation_get_phonemizer()` and `ethervox_conversation_get_tts()`
    return NULL (API compat stubs).
  - `settings_menu.c`: Voice testing functions stubbed with "no longer available" message;
    pronunciation reset action disabled.
  - `language_detector.c`: TTS reload calls removed; language switching now logs platform-managed TTS.
  - Excluded from build (depend on removed phonemizer): `voice_training.c`, `global_tts.c`,
    `train_pronunciation.c`.
  - Standalone CLI app (`main.c`): Still references old TTS API (not part of library build).
- **`include/ethervox/file_tools.h`** (TASK-C1.2): Added compile-time assertion that
  `ETHERVOX_FILE_ACCESS_READ_WRITE` cannot be selected under `ETHERVOX_PROFILE=WORKSPACE` (ADR-0007,
  AGENTS.md I2: no model output can mutate the file system in Workspace shells).

### Removed
- `src/tmp.txt` (tracked debug-log dump); `main.c.bak`/`tmp.txt` added to `.gitignore`.
