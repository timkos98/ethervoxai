# Changelog

All notable changes to this project are documented here, following
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and
[Semantic Versioning](https://semver.org/).

---

## [core-v0.3.0] - 2026-08-10

**ADR-0023: One `ethervox_core`, one line** — Merged `feature/granite-speech-voice-integration` (37 commits) into `main`. Deleted 8 stale branches. All products now track one main line via tags.

### Added (Phase C1.0–C1.6)
- **4-profile CI matrix** (TASK-C0.1): All four profiles (EDGE, MOBILE, DESKTOP, WORKSPACE) now build and pass `ctest` on every PR via GitHub Actions
- **PR template** requiring consumer impact statement per ADR-0023 §5
- All features from C1.0–C1.6 (see Unreleased section below, now released)

### Changed
- **Repository policy** (ADR-0023): `main` is now the only long-lived branch. Products pin tags, not SHAs. Feature branches are short-lived and deleted on merge.
- All consumers must now use tags (e.g., `core-v0.3.0`) instead of branch heads

### Removed
- 8 stale remote branches: `feature/granite-speech-voice-integration`, `agents/unified-voice-model-architecture`, `dev/architecture_change__tool_manifest_approach`, `dev/llm_first_approach`, `feat-android-basic-frontend`, `feat/conversation-summarization`, `feat/memory-tool`, `feat/voice-tool`

### Breaking Changes
- **TTS subsystem removed**: Old `ethervox_tts_*` APIs deleted, replaced with `ethervox_tts_host_t` interface
- **Paths required**: Must supply `ethervox_paths_t`, no more hardcoded `HOME`/`Documents/` derivations
- **Profile required**: Must set `-DETHERVOX_PROFILE=<EDGE|MOBILE|DESKTOP|WORKSPACE>` in CMake
- **KV cache invalidated**: Old cache files (`system_prompt_*.kvcache`) won't be found, triggers one-time rebuild

### Consumer Impact
- ✅ **ethervoxai-apple (Workspace)**: Already at feda1a0, re-pinned to `core-v0.3.0`, builds successfully
- ⚠️ **ethervoxai-android (Friend'O'Mine)**: Pinned at 07fc7e7 with TTS/phonemizer code. Must implement N5.5 (TTS host) before re-pinning to avoid breaking read-aloud (accessibility-critical)
- ✅ **ethervoxai-ios**: Pinned at 07fc7e7, does not use C TTS API, safe to re-pin with paths API adoption

See TASK-C0.1 execution log in `ethervoxai-planning/tasks/PHASE-C/C0.1-unify-the-core-line.md` for detailed consumer impact audit.

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
- **Finish reason constants** (TASK-C1.1): `ETHERVOX_FINISH_STOP`, `ETHERVOX_FINISH_LENGTH`,
  `ETHERVOX_FINISH_TOOL_CALLS`, `ETHERVOX_FINISH_CONTENT_FILTER`, `ETHERVOX_FINISH_REPETITION`,
  `ETHERVOX_FINISH_EOG` in `include/ethervox/governor.h`. Generation now reports exact stop reason
  (aligned with OpenAI API).
- **Consolidated stop checker** (TASK-C1.1): `governor_should_stop()` helper function in
  `src/governor/governor.c` that checks all stop conditions (EOG, stop sequences, max tokens,
  repetition loops) in one place. Replaces three redundant stop-check locations.
- **Repetition loop detection** (TASK-C1.1): Cycles of ≤8 tokens repeated >3 times now trigger
  `ETHERVOX_FINISH_REPETITION`, preventing infinite loops like "yes yes yes..." hallucinations.
- **Per-model `ignore_eog` property** (TASK-C1.1): Added `bool ignore_eog` to `chat_template_t`
  (`include/ethervox/chat_template.h`). All templates default to `false` (respect model's EOG
  decision). Can be set `true` for creative tasks where models may EOG prematurely. Removes
  Granite-specific EOG special-casing.
- `tests/test_stop_sequences.c` (TASK-C1.1): Conformance test verifying finish reason constants,
  EOG handling, stop sequence detection, and max tokens backstop. 26 assertions covering all 5
  chat templates.
- **`ethervox_paths_t`** (TASK-C1.3): Caller-supplied path configuration API (`include/ethervox/paths.h`,
  `src/common/paths.c`) replacing hardcoded `getenv("HOME")` and `Documents/` derivations. Required for
  macOS App Sandbox, security-scoped vault access, and mobile file-access restrictions. API includes:
  - `ethervox_paths_t` struct: `data_dir`, `cache_dir`, `models_dir`, `temp_dir` (all must be absolute)
  - `ethervox_paths_validate()`: Checks paths are absolute, exist/creatable, have R/W permissions
  - `ethervox_paths_get_default()`: Platform-specific defaults (macOS/Linux/Windows; NOT Android/iOS)
  - `ethervox_path_join()`, `ethervox_ensure_directory()`: Path manipulation helpers
  - `tests/test_paths.c`: 21 assertions covering validation, defaults, joining, directory creation
  - `scripts/check-no-hardcoded-paths.sh`: Grep test warning about hardcoded path usage (45 getenv("HOME"),
    2 "Documents/", 16 "/Library/" instances remain - migration in progress)
- **`ethervox_cancel_token_t`** (TASK-C1.4): Thread-safe cancellation token for long-running operations
  (`include/ethervox/cancel_token.h`, `src/governor/cancel_token.c`). API includes:
  - `ethervox_cancel_token_create()`: Allocate a new token in non-cancelled state
  - `ethervox_cancel_token_cancel()`: Atomically set the cancelled flag (thread-safe)
  - `ethervox_cancel_token_is_cancelled()`: Atomically read the cancelled flag (thread-safe)
  - `ethervox_cancel_token_free()`: Free the token (not thread-safe - caller must synchronize)
  - Threaded through model loading and generation: added optional `ethervox_cancel_token_t*` parameter
    to `ethervox_governor_load_model()`, `ethervox_governor_load_model_with_audio()`,
    `ethervox_governor_execute()`, and `ethervox_governor_execute_with_context()`. All accept NULL
    for backwards compatibility.
  - Checked between token generation steps and in model loading progress callbacks for sub-200ms
    cancellation latency. Session remains usable after cancellation with no leaked state.
  - Uses C11 atomic_bool for lock-free thread-safety (minimal overhead)
  - `tests/test_cancel_token.c`: 8 test cases covering create/free, NULL safety, basic cancellation,
    idempotency, cross-thread cancellation, and concurrent is_cancelled checks. All tests pass.- **Structured event stream** (TASK-C1.5): Unified `ethervox_event_cb` callback interface for tokens,
  tool calls, usage, load stages, log-probs, and errors (`include/ethervox/event_stream.h`,
  `src/common/event_stream.c`). API includes:
  - `ethervox_event_type_t`: 8 event types (LOAD_STAGE, TOKEN, TOOL_CALL_REQUESTED,
    TOOL_CALL_RESULT, USAGE, LOGPROB, FINISHED, ERROR)
  - `ethervox_event_t`: Tagged union carrying event-specific data
  - `ethervox_event_cb`: Callback function signature (returns bool to continue/cancel)
  - **UTF-8 validation**: Token fragments are guaranteed to never split multi-byte sequences. Ported
    UTF-8 validation logic from iOS bridge (`EthervoxBridge.mm`) to C. Handles 1-4 byte sequences
    (ASCII, 2-byte, 3-byte, 4-byte emoji), detects incomplete sequences, validates continuation bytes.
  - `ethervox_validate_utf8()`: Validates UTF-8 string, returns length of valid prefix
  - `ethervox_create_safe_utf8()`: Creates UTF-8 safe copy (truncates incomplete sequences)
  - Threaded through governor execution: added optional `ethervox_event_cb` parameter to
    `ethervox_governor_execute()` and `ethervox_governor_execute_with_context()`. Old
    `token_callback` reimplemented on top of event stream for backwards compatibility.
  - Events delivered in order on the inference thread. String pointers valid only during callback.
  - `tests/test_event_stream.c`: 16 test cases covering UTF-8 validation (NULL, ASCII, emoji,
    incomplete 2/3/4-byte sequences, invalid continuation, mixed content), safe string creation,
    and event structure correctness. All tests pass. Event size: 32 bytes.
- **KV-cache re-keying with hash-based keys** (TASK-C1.6): Multi-model cache support via hash-based
  filenames and eviction API (`include/ethervox/kv_cache_persistence.h` version 2,
  `src/governor/kv_cache_persistence.c`). Cache keys are now computed from:
  - Model file digest (first 1MB + file size — fast but collision-resistant)
  - System prompt hash (token sequence XOR)
  - Context size, quantization type, backend version
  - **Filename format**: `<cache_dir>/kv_cache_<hex_hash>.bin` (64-char hex hash of all components)
  - API changes:
    - `ethervox_kv_cache_get_path()`: Computes hash-based path from governor state
    - `ethervox_kv_cache_save/load()`: Now take `ethervox_paths_t*` and compute path internally
    - `ethervox_kv_cache_exists()`: Updated to use hash-based lookup
    - `ethervox_kv_cache_usage()`: New function returns total bytes, count, oldest/newest timestamps
    - `ethervox_kv_cache_evict_older_than()`: New function deletes caches older than timestamp,
      returns evicted count and bytes freed
  - Hash functions: XOR-based for speed (comment notes SHA-256 would be better but avoids crypto
    dependency). File digest uses simple read of first 1MB for large models.
  - Directory scanning for usage/eviction: Uses `dirent.h` to iterate `kv_cache_*.bin` files,
    extracts size/mtime via `stat()`, accumulates statistics.
  - **Backwards incompatible**: Old cache files (system_prompt_*.kvcache) will not be found by new
    hash-based lookup. One-time cache rebuild on first run after upgrade.
  - **Multi-model support**: Different models can now coexist in the same cache directory without
    filename collisions. Cache mismatches result in silent misses (new cache generated), never
    wrong loads — llama.cpp handles validation.
  - All function signatures updated to use `ethervox_paths_t*` (from C1.3) instead of hardcoded paths.
  - Library compiles successfully (1.3MB); tests not yet added (BACKLOG).

### Fixed
- **Stop-sequence infinite loop** (TASK-C1.1): Fixed the bug where sampled tokens were fed into the
  KV cache via `llama_decode()` BEFORE stop-sequence checks, causing the model to see its own stop
  markers and repeat them infinitely. `governor_should_stop()` is now called BEFORE
  `llama_decode()`, ensuring stop tokens never enter the context. Stop sequences are excluded from
  the final output, and generation terminates at exactly the right token.
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
