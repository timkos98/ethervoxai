# Voice Tools Design Document

**Status**: Design Phase  
**Target**: EthervoxAI Governor Integration  
**License Requirements**: MIT-compatible only (commercial-safe)

## Overview

This document outlines the design for integrating voice input/output capabilities into EthervoxAI, enabling the LLM to listen to conversations, transcribe speech, identify speakers, and respond with natural-sounding synthesized speech.

## Core Requirements

### Functional Requirements
1. **Text-to-Speech (TTS)**: Speak LLM responses with natural voice
2. **Speech-to-Text (STT)**: Transcribe user speech to text
3. **Dynamic Listening**: LLM-controlled or user-forced listening duration
4. **Speaker Detection**: Identify and separate different speakers
5. **Transcript Storage**: Full verbatim records in memory system
6. **Summary Generation**: LLM creates concise summaries from transcripts
7. **User Control**: `/listen`, `/stop`, `/tts` commands
8. **LLM Tool Access**: Governor tools for autonomous voice interaction

### Non-Functional Requirements
1. **Privacy**: All processing must be local (no cloud dependencies)
2. **Performance**: Real-time or near-real-time processing on CPU
3. **License**: MIT or Apache 2.0 only (commercial-safe)
4. **Platform Support**: macOS, Linux, Windows, Raspberry Pi
5. **Quality**: Natural-sounding TTS, accurate STT transcription
6. **Memory Efficient**: Run on modest hardware (8GB RAM minimum)

## Technology Stack

### Text-to-Speech: Piper (MIT License)

**Selected Implementation**: Original Piper (rhasspy/piper)
- **Repository**: https://github.com/rhasspy/piper
- **License**: MIT ✅
- **Technology**: VITS neural vocoder
- **Language**: C++ (onnxruntime)
- **Model Size**: 10-50MB per voice
- **Speed**: Real-time on CPU (>1.0x)
- **Quality**: Natural-sounding (8-9/10)
- **Voices**: 50+ languages, multiple speakers per language

**Why Piper**:
- Native C++ implementation (easy integration)
- Fast CPU inference (no GPU required)
- Small model files (embedded-friendly)
- MIT license (fully commercial)
- Proven stability (production-ready)

**Alternative Considered**: Piper1-GPL (rejected due to GPL-3.0 license)

### Speech-to-Text: Whisper.cpp (MIT License)

**Selected Implementation**: whisper.cpp (ggerganov/whisper.cpp)
- **Repository**: https://github.com/ggerganov/whisper.cpp
- **License**: MIT ✅
- **Technology**: OpenAI Whisper in C/C++
- **Model Sizes**: tiny (39M), base (74M), small (244M), medium (769M), large (1550M)
- **Speed**: Real-time with tiny/base models on CPU
- **Quality**: State-of-the-art accuracy
- **Features**: Multi-language, timestamps, word-level alignment

**Recommended Model**: `base.en` (74MB, English-only, ~4x real-time on M1)

**Why Whisper.cpp**:
- Industry standard for local STT
- Excellent accuracy-to-speed ratio
- Multi-language support
- Timestamp information for speaker segmentation
- Active maintenance
- MIT licensed

### Speaker Diarization Strategy

**Phase 1**: Simple Heuristics (No external dependencies)
- Voice energy level changes
- Pitch/frequency analysis
- Pause detection between speakers
- Basic clustering algorithm
- Accuracy: 60-70% (good enough for basic use)

**Phase 2** (Optional): Advanced Diarization
- pyannote.audio subprocess (MPL license - user installs separately)
- Voice fingerprinting with MFCC features
- External tool integration (user consent required)
- Accuracy: 90%+

**Implementation Note**: Start with Phase 1, make Phase 2 opt-in plugin

## Architecture Design

### Voice Engine Structure

```c
// include/ethervox/voice.h

#define ETHERVOX_VOICE_MAX_SPEAKERS 10
#define ETHERVOX_VOICE_BUFFER_SIZE 16000  // 1 second at 16kHz

typedef struct {
    char* text;               // Transcribed text
    float start_time;         // Segment start (seconds)
    float end_time;           // Segment end (seconds)
    uint32_t speaker_id;      // Speaker identifier (0-based)
    float confidence;         // Transcription confidence (0.0-1.0)
} ethervox_voice_segment_t;

typedef struct {
    // TTS Engine (Piper)
    void* piper_ctx;          // Piper synthesis context
    char tts_model_path[512]; // Path to voice model (.onnx)
    
    // STT Engine (Whisper.cpp)
    void* whisper_ctx;        // Whisper context
    char stt_model_path[512]; // Path to Whisper model
    
    // Audio I/O
    void* audio_input;        // Platform-specific input stream
    void* audio_output;       // Platform-specific output stream
    
    // State
    bool is_listening;        // Currently recording audio
    bool tts_enabled;         // Auto-speak LLM responses
    uint32_t listen_duration_ms; // Target listening duration
    time_t listen_start_time; // When listening started
    
    // Buffers
    float* audio_buffer;      // Raw audio samples
    size_t buffer_pos;        // Current buffer position
    size_t buffer_capacity;   // Total buffer size
    
    // Speaker tracking
    uint32_t num_speakers;    // Detected speakers count
    float speaker_profiles[ETHERVOX_VOICE_MAX_SPEAKERS][128]; // Voice prints
    
    // Configuration
    bool detect_speakers;     // Enable speaker diarization
    float vad_threshold;      // Voice activity detection threshold
    uint32_t sample_rate;     // Audio sample rate (16000 Hz)
    
} ethervox_voice_engine_t;
```

### Core API Functions

```c
/**
 * Initialize voice engine
 * 
 * @param engine Voice engine to initialize
 * @param tts_model_path Path to Piper voice model (.onnx)
 * @param stt_model_path Path to Whisper model (base.en.bin)
 * @param config Optional configuration (NULL = defaults)
 * @return 0 on success, negative on error
 */
int ethervox_voice_init(
    ethervox_voice_engine_t* engine,
    const char* tts_model_path,
    const char* stt_model_path,
    const ethervox_voice_config_t* config
);

/**
 * Text-to-Speech: Synthesize and play audio
 * 
 * @param engine Voice engine
 * @param text Text to speak
 * @param interruptible Can be stopped by user input
 * @return 0 on success, negative on error
 */
int ethervox_voice_speak(
    ethervox_voice_engine_t* engine,
    const char* text,
    bool interruptible
);

/**
 * Start listening to audio input
 * 
 * @param engine Voice engine
 * @param duration_ms Listen duration (0 = continuous until stopped)
 * @param detect_speakers Enable speaker diarization
 * @return 0 on success, negative on error
 */
int ethervox_voice_listen_start(
    ethervox_voice_engine_t* engine,
    uint32_t duration_ms,
    bool detect_speakers
);

/**
 * Stop listening and retrieve transcript
 * 
 * @param engine Voice engine
 * @param transcript_out Full combined transcript (caller must free)
 * @param segments_out Array of timestamped segments (caller must free)
 * @param num_segments Number of segments returned
 * @return 0 on success, negative on error
 */
int ethervox_voice_listen_stop(
    ethervox_voice_engine_t* engine,
    char** transcript_out,
    ethervox_voice_segment_t** segments_out,
    uint32_t* num_segments
);

/**
 * Check if currently listening
 */
bool ethervox_voice_is_listening(const ethervox_voice_engine_t* engine);

/**
 * Enable/disable auto-TTS for LLM responses
 */
void ethervox_voice_set_tts_enabled(ethervox_voice_engine_t* engine, bool enabled);

/**
 * Cleanup and free resources
 */
void ethervox_voice_cleanup(ethervox_voice_engine_t* engine);
```

## Governor Tool Integration

### Tool: voice_speak

```json
{
  "name": "voice_speak",
  "description": "Speak text aloud using text-to-speech. Use for important announcements, responses to voice queries, or to get user attention.",
  "parameters": {
    "text": {
      "type": "string",
      "description": "Text to speak aloud"
    },
    "interruptible": {
      "type": "boolean",
      "description": "Whether speech can be interrupted by user (default: true)",
      "default": true
    }
  },
  "required": ["text"]
}
```

**Example Usage**:
```xml
<tool_call name="voice_speak" text="Your build completed successfully!" />
```

### Tool: voice_listen

```json
{
  "name": "voice_listen",
  "description": "Listen to audio input and transcribe speech. Can detect multiple speakers. Returns timestamped transcript segments. Use when you need to capture spoken information, conduct voice interviews, or record meetings.",
  "parameters": {
    "duration_seconds": {
      "type": "integer",
      "description": "How long to listen in seconds (0 = continuous until user stops, 1-300 = fixed duration)",
      "minimum": 0,
      "maximum": 300,
      "default": 30
    },
    "detect_speakers": {
      "type": "boolean",
      "description": "Attempt to identify and separate different speakers",
      "default": true
    },
    "auto_summarize": {
      "type": "boolean",
      "description": "Automatically generate and store summary after transcription",
      "default": true
    }
  }
}
```

**Example Usage**:
```xml
<tool_call name="voice_listen" duration_seconds="60" detect_speakers="true" />
```

**Response Format**:
```json
{
  "success": true,
  "transcript": "Hello there. Hi, how are you? I'm doing well, thanks for asking.",
  "duration_actual": 8.5,
  "num_speakers": 2,
  "segments": [
    {
      "speaker": 0,
      "text": "Hello there.",
      "start": 0.5,
      "end": 1.8,
      "confidence": 0.95
    },
    {
      "speaker": 1,
      "text": "Hi, how are you?",
      "start": 2.1,
      "end": 3.5,
      "confidence": 0.92
    },
    {
      "speaker": 0,
      "text": "I'm doing well, thanks for asking.",
      "start": 3.8,
      "end": 5.9,
      "confidence": 0.94
    }
  ]
}
```

## User Commands

### /tts [on|off]
Enable or disable automatic text-to-speech for LLM responses.

```bash
> /tts on
TTS enabled - all responses will be spoken aloud

> /tts off
TTS disabled
```

### /listen [duration]
Force the LLM to start listening (user-initiated).

```bash
> /listen
Listening (continuous - use /stop to end)...

> /listen 30
Listening for 30 seconds...

> /listen 0
Listening (continuous)...
```

### /stop
Stop current listening session.

```bash
> /stop
Stopped listening. Transcribing...
[Transcript appears]
```

### /voice status
Show current voice engine status.

```bash
> /voice status
TTS: enabled (piper/en_US-lessac-medium.onnx)
STT: ready (whisper/base.en.bin)
Listening: no
Speakers detected: 0
```

## Memory Integration

### Transcript Storage

Voice transcripts are stored in the memory system with special formatting and tags:

```c
// Full transcript with timestamps and speaker labels
uint64_t transcript_id;
char transcript_text[ETHERVOX_MEMORY_MAX_TEXT_LEN];

snprintf(transcript_text, sizeof(transcript_text),
    "VOICE TRANSCRIPT [00:00-00:28] - %u speakers\n\n"
    "[Speaker 0, 00:00-00:02] Hello there\n"
    "[Speaker 1, 00:02-00:04] Hi how are you\n"
    "[Speaker 0, 00:05-00:15] I wanted to discuss the project timeline\n"
    "[Speaker 1, 00:16-00:25] Sure, what's your availability next week?\n"
    "[Speaker 0, 00:26-00:28] I'm free Tuesday afternoon",
    num_segments
);

const char* tags[] = {"voice", "transcript", "multi-speaker", "conversation"};
ethervox_memory_store_add(&memory_store, transcript_text, tags, 4, 
                         0.7, // Medium importance (raw data)
                         true, &transcript_id);
```

### Summary Storage

LLM generates and stores concise summary:

```c
// LLM uses memory_store tool after listening:
{
  "text": "Meeting summary: Two speakers discussed project timeline. 
           Speaker 0 requested availability for next week. 
           Speaker 1 asked about schedule. 
           Speaker 0 confirmed Tuesday afternoon availability.",
  "tags": ["voice", "summary", "meeting", "schedule"],
  "importance": 0.95, // High importance (processed insight)
  "is_user": false
}
```

### Cross-Referencing

Link summary to transcript:

```c
// Store relationship metadata
char metadata[256];
snprintf(metadata, sizeof(metadata),
    "Summary of transcript ID %llu", transcript_id);

// Append to summary text or use custom metadata field
```

## Implementation Phases

### Phase 1: TTS Output (Week 1)
**Goal**: LLM can speak responses

**Tasks**:
1. Integrate Piper library (compile as dependency)
2. Implement `ethervox_voice_init()` with TTS-only mode
3. Implement `ethervox_voice_speak()`
4. Add `/tts on/off` command to main.c
5. Register `voice_speak` tool with governor
6. Auto-speak LLM responses when TTS enabled

**Deliverables**:
- `src/voice/voice_tts.c` - TTS implementation
- `include/ethervox/voice.h` - Public API
- Piper model download script
- Documentation updates

**Testing**:
```bash
> /tts on
> Hello, can you speak this?
[LLM responds with text AND speaks it]
```

### Phase 2: Basic STT Input (Week 2)
**Goal**: Capture and transcribe single-speaker audio

**Tasks**:
1. Integrate whisper.cpp library
2. Implement audio capture (platform-specific)
3. Implement `ethervox_voice_listen_start()` - fixed duration
4. Implement `ethervox_voice_listen_stop()` - single speaker
5. Add `/listen` and `/stop` commands
6. Register `voice_listen` tool

**Deliverables**:
- `src/voice/voice_stt.c` - STT implementation
- `src/voice/audio_capture_*.c` - Platform audio I/O
- Whisper model download script
- Basic transcript formatting

**Testing**:
```bash
> /listen 10
Listening for 10 seconds...
[User speaks: "Test transcription"]
Transcript: Test transcription
```

### Phase 3: LLM-Controlled Listening (Week 3)
**Goal**: LLM decides when and how long to listen

**Tasks**:
1. Implement continuous listening mode (duration=0)
2. Add background thread for audio capture
3. Implement user interrupt handling
4. Update governor tool parameters
5. Test LLM autonomous listening

**Deliverables**:
- Thread-safe audio capture
- Interrupt mechanism
- Updated tool schemas

**Testing**:
```
User: "I need to tell you something"
LLM: <tool_call name="voice_listen" duration_seconds="30" />
[Listens and transcribes]
LLM: "I understand you said..."
```

### Phase 4: Speaker Detection (Week 4)
**Goal**: Identify and separate multiple speakers

**Tasks**:
1. Implement simple speaker detection heuristics
   - Voice energy level tracking
   - Pitch/frequency analysis
   - Pause-based segmentation
2. Assign speaker IDs to segments
3. Format multi-speaker transcripts
4. Update memory storage with speaker labels

**Deliverables**:
- `src/voice/speaker_detection.c`
- Enhanced transcript formatting
- Speaker profile tracking

**Testing**:
```bash
> /listen 20
[Two people speak]
Transcript:
[Speaker 0] Hello there
[Speaker 1] Hi how are you
[Speaker 0] I'm doing well
```

### Phase 5: Memory Integration & Summaries (Week 5)
**Goal**: Seamless voice data storage and LLM summarization

**Tasks**:
1. Automatic transcript storage after listening
2. LLM generates summaries using `memory_store` tool
3. Cross-reference summaries with transcripts
4. Search/retrieve voice memories
5. Export voice transcripts

**Deliverables**:
- Voice memory templates
- Summary generation prompts
- Search by speaker
- Export formats (text, SRT subtitles)

**Testing**:
```bash
> /listen 60
[Long conversation]
> Can you summarize what we discussed?
[LLM searches voice memories, generates summary]
```

## Platform-Specific Considerations

### macOS
- **Audio Input**: Core Audio (AudioQueue API)
- **Audio Output**: AVAudioEngine or play command
- **TTS Fallback**: `say` command (if Piper unavailable)
- **Permissions**: Microphone access (Info.plist entry)

### Linux
- **Audio Input**: ALSA or PulseAudio
- **Audio Output**: ALSA or PulseAudio
- **Dependencies**: `libasound2-dev`, `libpulse-dev`
- **Permissions**: User must be in `audio` group

### Windows
- **Audio Input**: WASAPI or WinMM
- **Audio Output**: WASAPI
- **TTS Fallback**: SAPI (if Piper unavailable)
- **Permissions**: Microphone access prompt

### Raspberry Pi
- **Audio Input**: ALSA (USB mic recommended)
- **Audio Output**: ALSA (3.5mm jack or HDMI)
- **Model Choice**: Whisper tiny (39MB) for CPU constraints
- **Performance**: ~2-3x real-time on RPi 4

## Resource Requirements

### Storage
- Piper voice model: 10-50 MB (per voice)
- Whisper base.en model: 74 MB
- Whisper tiny model: 39 MB (for RPi)
- Total minimum: ~100-150 MB

### Memory (RAM)
- Piper inference: ~50-100 MB
- Whisper inference: ~200-500 MB (base model)
- Audio buffers: ~10-50 MB
- Total minimum: ~500 MB dedicated

### CPU
- Piper TTS: 1-2 CPU seconds per sentence (real-time)
- Whisper STT: 3-5x real-time (base model, modern CPU)
- Speaker detection: Negligible overhead
- Total: 1-2 CPU cores recommended

### Recommended Hardware
- **Minimum**: Raspberry Pi 4 (4GB), Intel i5 (2015+), Apple M1
- **Optimal**: Any modern desktop/laptop (2020+)
- **Not Recommended**: Raspberry Pi 3, older ARM devices

## Security & Privacy

### Data Handling
- All audio processing happens **locally** (no network calls)
- No audio uploaded to cloud services
- Transcripts stored in local memory JSONL files
- User controls retention via `/clear` command

### Permissions
- Explicit microphone permission required
- User notified when listening starts
- Visual/audio indicator during recording
- Can disable voice features entirely

### Data Retention
- Transcripts: Stored in memory until explicitly cleared
- Audio buffers: Cleared immediately after transcription
- No raw audio files saved (unless user explicitly exports)
- Summaries: Retained per memory importance settings

## Testing Strategy

### Unit Tests
- TTS synthesis with various text inputs
- STT transcription accuracy benchmarks
- Speaker detection with synthetic test audio
- Memory storage and retrieval
- Command parsing

### Integration Tests
- Full voice conversation workflow
- LLM autonomous listening decisions
- Multi-speaker conversation handling
- Interrupt and resume scenarios
- Memory search across voice data

### Performance Tests
- Real-time factor measurement (RTF)
- Memory usage profiling
- CPU utilization monitoring
- Latency measurements (speak/listen)

### User Acceptance Tests
- Natural conversation flows
- Multi-speaker meeting scenarios
- Voice command accuracy
- TTS naturalness ratings
- Overall UX satisfaction

## Future Enhancements

### Post-MVP Features
1. **Voice Cloning**: Custom voice models (Piper supports training)
2. **Real-time Translation**: Whisper multi-language → English
3. **Emotion Detection**: Tone analysis in speech
4. **Wake Word**: "Hey Ethervox" activation
5. **Voice Commands**: "/remind me" spoken aloud
6. **Noise Cancellation**: Background noise filtering
7. **Echo Cancellation**: For speaker + mic scenarios
8. **Advanced Diarization**: pyannote.audio integration
9. **Voice Biometrics**: Speaker identification by voice print
10. **Streaming STT**: Real-time transcription display

### Research Areas
- On-device voice activity detection (WebRTC VAD)
- Low-latency streaming pipeline
- Custom Whisper fine-tuning for domain jargon
- Compressed voice models for embedded devices

## Dependencies & Licensing

### Direct Dependencies
| Library | License | Purpose | Commercial OK |
|---------|---------|---------|---------------|
| Piper | MIT | Text-to-speech | ✅ Yes |
| whisper.cpp | MIT | Speech-to-text | ✅ Yes |
| ggml | MIT | ML inference (Whisper) | ✅ Yes |
| onnxruntime | MIT | ML inference (Piper) | ✅ Yes |

### Transitive Dependencies
| Library | License | Purpose | Commercial OK |
|---------|---------|---------|---------------|
| ggml-metal | MIT | Metal acceleration | ✅ Yes |
| CoreAudio | Apple | macOS audio I/O | ✅ Yes |
| ALSA | LGPL | Linux audio (dynamic link) | ✅ Yes (if dynamic) |

### Optional Dependencies (User Installs)
| Library | License | Purpose | Commercial OK |
|---------|---------|---------|---------------|
| pyannote.audio | MPL 2.0 | Advanced speaker ID | ⚠️ Weak copyleft |

**License Summary**: Entire voice stack is MIT-licensed and commercially safe.

## References

### Documentation
- Piper GitHub: https://github.com/rhasspy/piper
- Piper Voice Samples: https://rhasspy.github.io/piper-samples/
- whisper.cpp: https://github.com/ggerganov/whisper.cpp
- Whisper Models: https://huggingface.co/ggerganov/whisper.cpp
- Speaker Diarization: https://github.com/pyannote/pyannote-audio

### Academic Papers
- VITS: Conditional Variational Autoencoder with Adversarial Learning for End-to-End Text-to-Speech (Kim et al., 2021)
- Robust Speech Recognition via Large-Scale Weak Supervision (Radford et al., 2022) - Whisper
- Neural Speaker Diarization with Speaker-Wise Chain Rule (Fujita et al., 2021)

### Model Downloads
- Piper voices: https://huggingface.co/rhasspy/piper-voices
- Whisper models: https://huggingface.co/ggerganov/whisper.cpp/tree/main

---

## Appendix A: Example Workflows

### Workflow 1: User Dictates Notes
```
User: /listen 60
System: Listening for 60 seconds...
User: [Speaks for 45 seconds about project ideas]
System: Stopped listening. Transcribing...

Transcript: [Full verbatim text]

Would you like me to summarize this?

User: Yes
LLM: <tool_call name="memory_store" text="Summary: Project brainstorming session. 
     Key ideas: 1) Focus on user privacy, 2) Local-first architecture, 
     3) Voice integration as differentiator" tags="notes,project,brainstorm" 
     importance="0.9" />

Summary stored. I've captured three main ideas from your dictation.
```

### Workflow 2: LLM Conducts Interview
```
User: Can you interview me about my project requirements?

LLM: I'd like to ask you some questions. May I listen to your responses?
<tool_call name="voice_listen" duration_seconds="0" detect_speakers="false" />

[Listening...]

LLM: What is the main problem you're trying to solve?
User: [Speaks answer]

LLM: <tool_call name="voice_listen" duration_seconds="0" />
[Continues asking and listening through multiple rounds]

LLM: Thank you. Let me summarize what I learned...
<tool_call name="memory_store" text="Requirements summary: ..." />
```

### Workflow 3: Meeting Transcription
```
User: /listen 1800
System: Listening for 30 minutes... (use /stop to end early)

[Two people have meeting]

User: /stop
System: Transcribing 28 minutes of audio...

Transcript:
[Speaker 0, 00:15] Good morning, let's discuss the roadmap
[Speaker 1, 00:18] Sounds good, I have the slides ready
...

LLM: I've transcribed your meeting. Would you like me to:
1. Generate action items
2. Create a summary
3. Identify key decisions

User: All three

LLM: <tool_call name="memory_store" text="Action items: 1) User to send slides by Friday..." />
<tool_call name="memory_store" text="Meeting summary: Roadmap discussion..." />
<tool_call name="memory_store" text="Decisions: Approved Q1 timeline..." />
```

---

**Document Version**: 1.0  
**Last Updated**: November 26, 2025  
**Author**: EthervoxAI Development Team  
**Status**: Design Complete - Ready for Implementation
