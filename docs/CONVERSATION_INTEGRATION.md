# Voice Conversation Integration - Complete

## Summary

Successfully integrated Governor LLM processing into the voice conversation pipeline. The system now:

1. ✅ **Listens** - Wake word or manual trigger activates listening
2. ✅ **Transcribes** - Whisper STT converts speech to text
3. ✅ **Processes** - Governor LLM generates intelligent response
4. ⏳ **Speaks** - Piper TTS (TODO - prints response for now)

## Changes Made

### 1. Governor Integration (`src/dialogue/voice_conversation.c`)

**Before**: Response processing was commented out with TODO
```c
// TODO: Send to Governor and get response
char* llm_response = NULL;
if (session->governor) {
    // ethervox_governor_submit_message(session->governor, recognized_text);
    // llm_response = ethervox_governor_get_response(session->governor);
}
```

**After**: Full Governor integration with proper API
```c
char* llm_response = NULL;
char* error_msg = NULL;

ethervox_governor_status_t status = ethervox_governor_execute(
    session->governor,
    recognized_text,
    &llm_response,
    &error_msg,
    NULL, NULL, NULL, NULL
);

if (status == ETHERVOX_GOVERNOR_SUCCESS && llm_response) {
    printf("\n🤖 Assistant: %s\n", llm_response);
    free(llm_response);
}
```

### 2. Global Governor Access (`src/main.c`)

**Added**:
- Global `g_governor` pointer (shared between CLI and conversation)
- Global `g_governor_mutex` for thread-safe access
- Updated `/convon` command to pass governor to conversation session

**Before**:
```c
// Local variable in main()
ethervox_governor_t* governor = NULL;

// Conversation initialized with NULL
g_conversation_session = ethervox_conversation_init(&config, NULL);
```

**After**:
```c
// Global variable
static ethervox_governor_t* g_governor = NULL;
static pthread_mutex_t g_governor_mutex = PTHREAD_MUTEX_INITIALIZER;

// Conversation initialized with governor
g_conversation_session = ethervox_conversation_init(&config, g_governor);
```

### 3. Better Error Handling

Added better error messages for:
- Governor not initialized when `/convon` is called
- Governor execution failures with error details
- Microphone busy scenarios in audio tests

## Testing

### Manual Test Flow

```bash
# Start EthervoxAI
./build/ethervoxai

# Load a model (if not autoloaded)
> /load ~/.ethervox/models/governor/granite-3.0-2b-instruct-Q4_K_M.gguf

# Enable conversation
> /convon
✓ Conversation session initialized with Governor
✓ Voice conversation ENABLED

# Enable wake word (optional)
> /wakeon
[Wake] ✓ Background calibration complete - actively listening for 'hey ethervox'

# Test manually
> /convtrigger
🎤 Manually triggering conversation...
Speak now! The system will listen for 5 seconds.

# Say something like "Hello, how's it going?"
# Expected output:
[Conversation] Final recognition: [Speaker 0]  Hello, how's it going?
[Conversation] Processing user input with Governor: [Speaker 0]  Hello, how's it going?

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
🤖 Assistant: Hello! I'm doing well, thank you for asking. How can I help you today?
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

### Wake Word Test

```bash
# After /wakeon and /convon are enabled
# Say "hey ethervox" → should auto-trigger conversation
# Then speak your question/command
```

## Current State

| Component | Status | Notes |
|-----------|--------|-------|
| Wake word detection | ✅ Working | Background thread monitors mic |
| Whisper STT | ✅ Working | 3s chunks, streaming mode |
| Governor LLM | ✅ Integrated | Full execute API with error handling |
| Piper TTS | ⏳ TODO | Response printed to console for now |
| Audio playback | ⏳ TODO | Needs output AudioQueue implementation |

## Next Steps

### 1. Piper TTS Backend (Priority: HIGH)
**Estimated Time**: 4-6 hours

Create `src/tts/piper_backend.c`:
- Load ONNX models from `~/.ethervox/models/piper/`
- Text → phonemes conversion
- Audio synthesis (22050 Hz float array)
- Return audio buffer for playback

### 2. Audio Playback (Priority: HIGH)  
**Estimated Time**: 2 hours

Extend `src/audio/platform_macos.c`:
- Add `start_playback()` function
- Create output AudioQueue (similar to input queue)
- Handle 22050 Hz → device sample rate conversion
- Blocking playback until complete

### 3. End-to-End Testing (Priority: MEDIUM)
**Estimated Time**: 1 hour

Test complete flow:
```
Wake word → Whisper STT → Governor → Piper TTS → Speakers
```

### 4. Governor Mutex Protection (Priority: MEDIUM)
**Estimated Time**: 1 hour

Currently the conversation thread calls Governor without locking. Need to add:
```c
pthread_mutex_lock(&g_governor_mutex);
status = ethervox_governor_execute(...);
pthread_mutex_unlock(&g_governor_mutex);
```

This prevents conflicts if user types commands while conversation is processing.

### 5. Conversation Memory Integration (Priority: LOW)
**Estimated Time**: 2 hours

- Store conversation turns in memory system
- Add conversation context to Governor system prompt
- Enable multi-turn conversations with context

## Known Issues

### 1. Wake Word Not Triggering Conversation

**Status**: Should be working now with g_governor integration

**Debug Steps**:
```bash
# Check if both are enabled
> /wakeon
> /convon

# Watch for detection message
[Wake] ✓ Background calibration complete - actively listening for 'hey ethervox'

# Say wake word - should see:
🎤 Wake word detected! (confidence: 0.85)
[Conversation] Thread activated, starting speech processing...
```

**If still not working**:
- Check `g_conversation_session != NULL` in wake word thread
- Verify `ethervox_conversation_trigger()` is called
- Add debug prints in conversation_thread when triggered

### 2. Governor Response Not Showing

**Possible Causes**:
1. Model not loaded (`/load` command first)
2. Governor not initialized when `/convon` called
3. Error in Governor execution (check logs)

**Debug**:
```c
// Add to voice_conversation.c after execute:
ETHERVOX_LOG_INFO("Governor status: %d, response: %s", 
                  status, llm_response ? llm_response : "NULL");
```

### 3. Audio Device Busy

**Symptom**: Tests fail with "Permission denied or device busy"

**Cause**: Microphone already in use by another application

**Solution**: Close EthervoxAI or other apps using the mic

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     Voice Conversation Flow                  │
└─────────────────────────────────────────────────────────────┘

1. TRIGGER
   ├─ Wake Word: "hey ethervox" detected
   └─ Manual: /convtrigger command

2. LISTEN (State: LISTENING)
   ├─ CoreAudio captures 200ms chunks
   ├─ Whisper STT processes in 3s windows
   └─ Timeout: 5 seconds of silence

3. PROCESS (State: PROCESSING)
   ├─ Final transcript extracted
   ├─ Governor execute() called
   └─ Response generated

4. SPEAK (State: SPEAKING)
   ├─ [TODO] Piper TTS synthesizes audio
   └─ [TODO] Audio playback to speakers

5. IDLE (State: IDLE)
   └─ Wait for next trigger
```

## Thread Safety

### Current Protection
- ✅ Wake word runtime: `g_wake_mutex`
- ✅ Conversation session: internal `session->mutex`
- ⏳ Governor: `g_governor_mutex` (added but not used yet)

### Need Protection
The conversation thread currently calls Governor without locking:
```c
// src/dialogue/voice_conversation.c:323
status = ethervox_governor_execute(session->governor, ...);  // ⚠️ Not thread-safe
```

Should be:
```c
pthread_mutex_lock(&g_governor_mutex);
status = ethervox_governor_execute(session->governor, ...);
pthread_mutex_unlock(&g_governor_mutex);
```

But `g_governor_mutex` is in main.c and not accessible from voice_conversation.c. Solutions:

**Option A**: Pass mutex to conversation_init()
```c
ethervox_conversation_session_t* ethervox_conversation_init(
    const ethervox_conversation_config_t* config,
    ethervox_governor_t* governor,
    pthread_mutex_t* governor_mutex  // NEW
);
```

**Option B**: Governor handles its own locking internally
- Best solution for encapsulation
- Requires changes to governor.c

## Files Modified

1. `src/dialogue/voice_conversation.c` - Governor integration
2. `src/main.c` - Global governor, /convon command updates
3. `src/audio/platform_macos.c` - Better error messages
4. `tests/unit/test_audio_integration.c` - Device busy handling
5. `tests/CMakeLists.txt` - Test suite integration
6. `docs/TESTING.md` - Testing documentation

## Commit Message

```
feat(conversation): integrate Governor LLM processing

- Add full Governor execute() integration in voice conversation
- Make governor global for CLI and conversation access
- Add thread-safe mutex for governor access (TODO: use in conv thread)
- Update /convon command to pass governor to session
- Improve error handling and logging

Speech now flows: Wake word → Whisper STT → Governor → (Print response)
Next: Piper TTS + audio playback

Fixes #voice-conversation
```
