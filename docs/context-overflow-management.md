# Mid-Generation Context Overflow Management

**Feature**: Automatic Context Window Management  
**Status**: Design Phase  
**Priority**: High  
**Target**: Governor LLM System  
**Author**: EthervoxAI Development Team  
**Last Updated**: November 27, 2025

---

## Table of Contents

1. [Problem Statement](#problem-statement)
2. [Solution Overview](#solution-overview)
3. [Why This Matters](#why-this-matters)
4. [Technical Architecture](#technical-architecture)
5. [Implementation Tasks](#implementation-tasks)
6. [Testing Strategy](#testing-strategy)
7. [Success Criteria](#success-criteria)
8. [Future Enhancements](#future-enhancements)

---

## Problem Statement

### Current Behavior

When the LLM's context window (currently 8,192 tokens) fills up during a conversation:

```
User: [sends message that would push context to 8,300 tokens]
System: ERROR: Context window exceeded - conversation too long
[Conversation terminates, all context lost]
```

**Location**: `src/governor/governor.c`, lines 622-627

```c
if (governor->current_kv_pos + n_tokens > n_ctx) {
    GOV_ERROR("Context window exceeded: current_pos=%d + new_tokens=%d > n_ctx=%d",
             governor->current_kv_pos, n_tokens, n_ctx);
    free(tokens);
    if (error) *error = strdup("Context window exceeded - conversation too long");
    return ETHERVOX_GOVERNOR_ERROR;
}
```

### Impact

- **User Experience**: Abrupt conversation termination
- **Data Loss**: All conversation context discarded
- **Friction**: User must manually restart and re-establish context
- **Commercial Risk**: Competitors handle this gracefully (ChatGPT, Claude)

### Real-World Scenario

```
User: "Let's discuss the project architecture" 
[30 minutes of detailed conversation - 7,500 tokens]
User: "Now summarize our decisions"
System: ERROR: Context window exceeded
User: 😡 [loses all context, must start over]
```

---

## Solution Overview

### Strategy: LLM-Driven Context Management

Instead of failing when context fills up, **empower the LLM to manage its own context** using a specialized tool.

### High-Level Flow

```
1. Monitor context usage continuously
2. When approaching limit (80%), inject system notification
3. LLM automatically calls context_manage tool
4. Tool executes: summarize old content, shift KV cache, store in memory
5. Conversation continues seamlessly
6. User sees brief notification (optional)
```

### Example After Implementation

```
User: "Let's discuss the project architecture"
[30 minutes, 7,500 tokens]
System: [Internal: Context at 85%, triggering management...]
LLM: <tool_call name="context_manage" action="summarize_old" keep_last_n_turns="10" />
System: [Summarizes turns 1-20, keeps turns 21-30, frees 3,500 tokens]
System: [Notification] Context managed: Summarized earlier discussion (3.5KB freed)

User: "Now summarize our decisions"
LLM: "Based on our discussion, here are the key decisions..." ✅
```

---

## Why This Matters

### 1. **User Experience**

**Before**: Hard failure, conversation reset  
**After**: Transparent management, continuous conversation

### 2. **Data Preservation**

**Before**: All context lost  
**After**: Old context summarized and stored in memory system, retrievable later

### 3. **Competitive Parity**

- ChatGPT: Handles long conversations gracefully
- Claude: 200K context window (expensive)
- **EthervoxAI**: 8K context BUT with smart management = effectively unlimited

### 4. **Commercial Viability**

Customers expect:
- Long-running conversations (hours/days)
- Context retention across sessions
- No arbitrary limits

### 5. **Memory System Integration**

Leverages existing memory infrastructure:
- Summaries stored as high-importance memories
- Searchable across sessions
- Automatic importance-based retention

---

## Technical Architecture

### Component 1: Context Health Monitoring

**Purpose**: Continuously track how full the context window is

**Implementation**: Add to `ethervox_governor` struct

```c
typedef enum {
    CTX_HEALTH_OK,           // 0-60% full - normal operation
    CTX_HEALTH_WARNING,      // 60-80% full - start planning
    CTX_HEALTH_CRITICAL,     // 80-95% full - must act now
    CTX_HEALTH_OVERFLOW      // >95% full - emergency fallback
} context_health_t;

typedef struct {
    context_health_t current_health;
    uint32_t overflow_event_count;
    llama_pos last_gc_position;
    bool management_in_progress;
} context_manager_state_t;
```

**Check Function**:

```c
context_health_t check_context_health(ethervox_governor_t* gov) {
    int n_ctx = llama_n_ctx(gov->llm_ctx);
    int current_pos = gov->current_kv_pos;
    float usage = (float)current_pos / n_ctx;
    
    if (usage < 0.60f) return CTX_HEALTH_OK;
    if (usage < 0.80f) return CTX_HEALTH_WARNING;
    if (usage < 0.95f) return CTX_HEALTH_CRITICAL;
    return CTX_HEALTH_OVERFLOW;
}
```

**Where to Call**: Before decoding new tokens (line ~618 in governor.c)

**Why**: Proactive detection allows graceful handling before emergency

---

### Component 2: Conversation Turn Tracking

**Purpose**: Know which KV cache positions correspond to which conversation turns

**Why Needed**: To selectively remove/summarize specific parts of conversation

**Data Structure**:

```c
typedef struct {
    uint32_t turn_number;      // Sequence number
    llama_pos kv_start;        // First token position
    llama_pos kv_end;          // Last token position
    time_t timestamp;          // When this turn occurred
    float importance;          // Estimated importance (0.0-1.0)
    bool is_user;              // User vs assistant turn
    char preview[128];         // First 128 chars for debugging
} conversation_turn_t;

typedef struct {
    conversation_turn_t* turns;
    uint32_t turn_count;
    uint32_t capacity;
} conversation_history_t;
```

**Tracking**: Update during `ethervox_governor_execute()`

```c
// When adding user message
conversation_turn_t user_turn = {
    .turn_number = governor->turn_counter++,
    .kv_start = governor->current_kv_pos,
    .kv_end = governor->current_kv_pos + n_user_tokens,
    .timestamp = time(NULL),
    .importance = estimate_turn_importance(user_query),
    .is_user = true
};
strncpy(user_turn.preview, user_query, 127);
append_turn(&governor->conversation_history, &user_turn);

// Update current position
governor->current_kv_pos += n_user_tokens;
```

**Why Important**: Enables surgical removal of specific conversation ranges

---

### Component 3: Context Management Tool

**Purpose**: Give LLM the ability to manage its own context

**Tool Definition**:

```json
{
  "name": "context_manage",
  "description": "Manage context window when running low on space. CRITICAL: You MUST call this when context usage exceeds 80%. Choose what to preserve and what to summarize based on conversation importance.",
  "parameters": {
    "action": {
      "type": "string",
      "enum": ["summarize_old", "shift_window", "prune_unimportant"],
      "description": "Action: summarize_old (best - creates searchable summary), shift_window (fast - drops oldest), prune_unimportant (selective removal)"
    },
    "keep_last_n_turns": {
      "type": "integer",
      "description": "Number of recent conversation turns to keep verbatim (10-20 recommended)",
      "minimum": 5,
      "maximum": 50,
      "default": 10
    },
    "summary_detail": {
      "type": "string",
      "enum": ["brief", "moderate", "detailed"],
      "description": "How detailed should the summary be (brief=200 tokens, moderate=500, detailed=1000)",
      "default": "moderate"
    }
  },
  "required": ["action"]
}
```

**Example LLM Usage**:

```xml
[System detects context at 85%]
<system>ALERT: Context usage at 85% (6,963/8,192 tokens). Call context_manage tool immediately.</system>

[LLM responds]
<tool_call name="context_manage" action="summarize_old" keep_last_n_turns="12" summary_detail="moderate" />

[Tool executes, returns]
{"success": true, "tokens_freed": 3420, "summary_stored": true, "memory_id": 1542}

[LLM continues normally]
Understood. I've preserved our earlier discussion in memory. Now, to answer your question...
```

---

### Component 4: Action Implementations

#### Action 1: `summarize_old`

**Best option** - Preserves information while freeing space

```c
int context_action_summarize_old(
    ethervox_governor_t* governor,
    ethervox_memory_store_t* memory_store,
    uint32_t keep_last_n_turns,
    const char* detail_level  // "brief", "moderate", "detailed"
) {
    // 1. Identify turns to summarize
    uint32_t total_turns = governor->conversation_history.turn_count;
    uint32_t turns_to_summarize = total_turns - keep_last_n_turns;
    
    if (turns_to_summarize <= 0) {
        return 0;  // Nothing to summarize
    }
    
    // 2. Extract text from those turns
    char* combined_text = malloc(32768);  // 32KB buffer
    size_t text_pos = 0;
    
    for (uint32_t i = 0; i < turns_to_summarize; i++) {
        conversation_turn_t* turn = &governor->conversation_history.turns[i];
        
        // Reconstruct text from KV cache or stored history
        const char* speaker = turn->is_user ? "User" : "Assistant";
        text_pos += snprintf(combined_text + text_pos, 32768 - text_pos,
                            "\n[Turn %u, %s]: %s",
                            turn->turn_number, speaker, turn->preview);
    }
    
    // 3. Generate summary using LLM or memory_summarize tool
    char* summary = generate_summary_internal(
        governor,
        combined_text,
        detail_level
    );
    
    // 4. Store summary in memory system
    const char* tags[] = {"context_summary", "auto_generated", "conversation"};
    uint64_t summary_id;
    
    ethervox_memory_store_add(
        memory_store,
        summary,
        tags,
        3,  // tag count
        0.95f,  // High importance
        false,  // Not user message
        &summary_id
    );
    
    // 5. Remove old turns from KV cache
    llama_pos remove_start = governor->conversation_history.turns[0].kv_start;
    llama_pos remove_end = governor->conversation_history.turns[turns_to_summarize - 1].kv_end;
    
    llama_kv_cache_seq_rm(governor->llm_ctx, 0, remove_start, remove_end);
    
    // 6. Shift remaining KV cache forward
    llama_pos shift_amount = remove_end - remove_start;
    llama_kv_cache_seq_shift(governor->llm_ctx, 0, remove_end, -shift_amount);
    
    // 7. Update turn tracking
    for (uint32_t i = turns_to_summarize; i < total_turns; i++) {
        governor->conversation_history.turns[i - turns_to_summarize] = 
            governor->conversation_history.turns[i];
        // Update KV positions
        governor->conversation_history.turns[i - turns_to_summarize].kv_start -= shift_amount;
        governor->conversation_history.turns[i - turns_to_summarize].kv_end -= shift_amount;
    }
    governor->conversation_history.turn_count -= turns_to_summarize;
    
    // 8. Update current position
    governor->current_kv_pos -= shift_amount;
    
    free(combined_text);
    free(summary);
    
    return shift_amount;  // Tokens freed
}
```

**Why This Works**:
- Preserves information (searchable summary in memory)
- Frees significant space
- LLM can retrieve summary later if needed
- User doesn't lose context

#### Action 2: `shift_window`

**Fast option** - Drop oldest content, no summary

```c
int context_action_shift_window(
    ethervox_governor_t* governor,
    uint32_t keep_last_n_turns
) {
    // Simple: remove oldest turns, shift everything
    // Faster but loses information
    
    uint32_t total_turns = governor->conversation_history.turn_count;
    uint32_t turns_to_drop = total_turns - keep_last_n_turns;
    
    if (turns_to_drop <= 0) return 0;
    
    llama_pos drop_end = governor->conversation_history.turns[turns_to_drop - 1].kv_end;
    
    llama_kv_cache_seq_rm(governor->llm_ctx, 0, 0, drop_end);
    llama_kv_cache_seq_shift(governor->llm_ctx, 0, drop_end, -drop_end);
    
    // Update tracking (same as summarize_old steps 7-8)
    
    return drop_end;
}
```

**Use Case**: When speed matters more than preservation

#### Action 3: `prune_unimportant`

**Selective option** - Remove low-importance turns

```c
int context_action_prune_unimportant(
    ethervox_governor_t* governor,
    float importance_threshold  // e.g., 0.5
) {
    // Remove turns with importance < threshold
    // More complex: need to handle gaps in KV cache
    
    // Implementation similar to summarize_old but:
    // 1. Filter turns by importance
    // 2. Remove non-contiguous ranges
    // 3. Compact KV cache
    
    // Note: llama.cpp doesn't support arbitrary gap removal well,
    // so this is more theoretical - would need creative KV manipulation
}
```

**Status**: Advanced feature, implement later

---

### Component 5: Automatic Trigger System

**Purpose**: Inject system message when context critical

**Implementation**: In `ethervox_governor_execute()` before LLM generation

```c
// Check context health before generating response
context_health_t health = check_context_health(governor);

if (health == CTX_HEALTH_CRITICAL && !governor->context_manager.management_in_progress) {
    // Inject system warning
    const char* warning_msg = 
        "\n<system>CRITICAL: Context usage at 85%. "
        "You MUST call context_manage tool before responding. "
        "Recommended: action='summarize_old', keep_last_n_turns=10</system>\n";
    
    // Tokenize and add to KV cache
    llama_token* warning_tokens = tokenize(warning_msg);
    decode_tokens(governor, warning_tokens, n_warning_tokens);
    
    governor->context_manager.management_in_progress = true;
}
```

**Why Automatic**: User shouldn't need to think about it

---

### Component 6: Summary Generation

**Two Approaches**:

#### Approach A: Use Existing Memory Tool

```c
// Leverage existing memory_summarize
char* generate_summary_internal(
    ethervox_governor_t* governor,
    const char* text,
    const char* detail_level
) {
    // Call memory_summarize with conversation text
    // Simple, reuses existing code
    
    // Problem: memory_summarize expects turn windows, not raw text
}
```

#### Approach B: Direct LLM Call (Recommended)

```c
char* generate_summary_internal(
    ethervox_governor_t* governor,
    const char* text,
    const char* detail_level
) {
    // Create temporary prompt
    char prompt[65536];
    int max_tokens = (strcmp(detail_level, "brief") == 0) ? 200 :
                     (strcmp(detail_level, "moderate") == 0) ? 500 : 1000;
    
    snprintf(prompt, sizeof(prompt),
        "<|im_start|>system\n"
        "Summarize this conversation concisely in %d tokens max. "
        "Preserve key facts, decisions, and context.\n"
        "<|im_end|>\n"
        "<|im_start|>user\n"
        "%s\n"
        "<|im_end|>\n"
        "<|im_start|>assistant\n",
        max_tokens, text
    );
    
    // Generate using current model (lightweight call)
    char* summary = llama_generate_simple(
        governor->llm_ctx,
        prompt,
        max_tokens
    );
    
    return summary;
}
```

**Why Better**: More flexible, controlled output length

---

## Implementation Tasks

### Phase 1: Foundation (Week 1)

**Task 1.1**: Add context manager state to governor

- [ ] Add `context_manager_state_t` to `ethervox_governor` struct
- [ ] Initialize in `ethervox_governor_init()`
- [ ] Cleanup in `ethervox_governor_cleanup()`

**File**: `src/governor/governor.c`, `include/ethervox/governor.h`

**Task 1.2**: Implement context health monitoring

- [ ] Add `check_context_health()` function
- [ ] Call before token decoding (line ~618)
- [ ] Log health transitions (OK→WARNING→CRITICAL)

**File**: `src/governor/governor.c`

**Task 1.3**: Add conversation turn tracking

- [ ] Add `conversation_history_t` to governor struct
- [ ] Track turns in `ethervox_governor_execute()`
- [ ] Store turn boundaries (kv_start, kv_end)
- [ ] Implement `append_turn()` helper

**File**: `src/governor/governor.c`

**Validation**: Compile successfully, basic logging works

---

### Phase 2: Tool Implementation (Week 2)

**Task 2.1**: Create `context_manage` tool wrapper

- [ ] Add to `src/plugins/context_tools/context_manage.c` (new file)
- [ ] Implement tool wrapper function
- [ ] Parse JSON parameters (action, keep_last_n_turns, summary_detail)
- [ ] Register with governor tool registry

**Files**: New `src/plugins/context_tools/`, update CMakeLists.txt

**Task 2.2**: Implement `summarize_old` action

- [ ] Extract text from conversation turns
- [ ] Generate summary using direct LLM call
- [ ] Store summary in memory system
- [ ] Remove old KV cache ranges
- [ ] Shift remaining cache forward
- [ ] Update turn tracking

**File**: `src/plugins/context_tools/context_actions.c`

**Task 2.3**: Implement `shift_window` action

- [ ] Calculate drop amount
- [ ] Remove from KV cache
- [ ] Shift remaining cache
- [ ] Update tracking

**File**: `src/plugins/context_tools/context_actions.c`

**Validation**: Manual tool call works, frees expected tokens

---

### Phase 3: Automatic Triggering (Week 3)

**Task 3.1**: Inject system warning at critical threshold

- [ ] Check health before LLM generation
- [ ] Inject warning message when CRITICAL
- [ ] Set `management_in_progress` flag
- [ ] Clear flag after tool completes

**File**: `src/governor/governor.c`

**Task 3.2**: Handle emergency overflow (>95%)

- [ ] Fallback to forced `shift_window` if LLM doesn't respond
- [ ] Log emergency events
- [ ] Notify user of emergency action

**File**: `src/governor/governor.c`

**Validation**: Long conversation automatically triggers management

---

### Phase 4: User Interface (Week 4)

**Task 4.1**: Add `/context` commands

- [ ] `/context status` - Show current usage, health
- [ ] `/context summarize` - Force summarization
- [ ] `/context clear` - Clear with confirmation

**File**: `src/main.c`

**Task 4.2**: Add status display

- [ ] Show context percentage in prompt
- [ ] Optional: Progress bar for context usage
- [ ] Brief notification when auto-management occurs

**File**: `src/main.c`

**Validation**: Commands work, status accurate

---

### Phase 5: Integration & Polish (Week 5)

**Task 5.1**: Memory integration

- [ ] Ensure summaries properly tagged
- [ ] Test cross-session summary retrieval
- [ ] Verify importance-based retention

**File**: `src/plugins/context_tools/context_actions.c`

**Task 5.2**: Configuration options

- [ ] Add context management settings to config
- [ ] Allow disabling auto-management
- [ ] Configurable thresholds

**File**: `include/ethervox/config.h`

**Task 5.3**: Documentation

- [ ] Update user guide
- [ ] Add architecture notes
- [ ] Document configuration options

**Files**: `docs/`

**Validation**: End-to-end testing, all features work

---

## Testing Strategy

### Unit Tests

**File**: `tests/test_context_overflow.c`

#### Test 1: Health Detection

```c
void test_context_health_detection() {
    // Setup
    ethervox_governor_t* gov = create_test_governor(8192);
    
    // Test OK state (30% full)
    set_kv_position(gov, 2457);
    assert(check_context_health(gov) == CTX_HEALTH_OK);
    
    // Test WARNING state (70% full)
    set_kv_position(gov, 5734);
    assert(check_context_health(gov) == CTX_HEALTH_WARNING);
    
    // Test CRITICAL state (85% full)
    set_kv_position(gov, 6963);
    assert(check_context_health(gov) == CTX_HEALTH_CRITICAL);
    
    // Test OVERFLOW state (97% full)
    set_kv_position(gov, 7946);
    assert(check_context_health(gov) == CTX_HEALTH_OVERFLOW);
    
    cleanup_test_governor(gov);
}
```

#### Test 2: Turn Boundary Tracking

```c
void test_turn_tracking() {
    ethervox_governor_t* gov = create_test_governor(8192);
    
    // Add 5 turns
    add_test_turn(gov, "Hello", true, 10);   // User, 10 tokens
    add_test_turn(gov, "Hi there", false, 8); // Asst, 8 tokens
    add_test_turn(gov, "How are you?", true, 12);
    add_test_turn(gov, "I'm well", false, 7);
    add_test_turn(gov, "Great!", true, 5);
    
    // Verify turn boundaries
    assert(gov->conversation_history.turn_count == 5);
    assert(gov->conversation_history.turns[0].kv_start == 0);
    assert(gov->conversation_history.turns[0].kv_end == 10);
    assert(gov->conversation_history.turns[1].kv_start == 10);
    assert(gov->conversation_history.turns[1].kv_end == 18);
    assert(gov->conversation_history.turns[4].kv_end == 42);
    assert(gov->current_kv_pos == 42);
    
    cleanup_test_governor(gov);
}
```

#### Test 3: Shift Window Action

```c
void test_shift_window() {
    ethervox_governor_t* gov = create_test_governor(8192);
    
    // Fill with 20 turns
    for (int i = 0; i < 20; i++) {
        add_test_turn(gov, "Test message", i % 2 == 0, 50);
    }
    
    assert(gov->current_kv_pos == 1000);  // 20 * 50
    
    // Shift window, keep last 5 turns
    int tokens_freed = context_action_shift_window(gov, 5);
    
    assert(tokens_freed == 750);  // 15 turns * 50 tokens
    assert(gov->current_kv_pos == 250);  // 5 turns * 50
    assert(gov->conversation_history.turn_count == 5);
    
    cleanup_test_governor(gov);
}
```

#### Test 4: Summarize Old Action

```c
void test_summarize_old() {
    ethervox_governor_t* gov = create_test_governor(8192);
    ethervox_memory_store_t* memory = create_test_memory();
    
    // Fill with conversation
    add_test_turn(gov, "User asks about Python", true, 50);
    add_test_turn(gov, "Assistant explains basics", false, 100);
    add_test_turn(gov, "User asks about lists", true, 40);
    add_test_turn(gov, "Assistant explains lists", false, 120);
    add_test_turn(gov, "Recent: debugging help", true, 60);
    add_test_turn(gov, "Recent: here's the fix", false, 80);
    
    // Summarize first 4 turns, keep last 2
    int tokens_freed = context_action_summarize_old(
        gov, memory, 2, "brief"
    );
    
    assert(tokens_freed == 310);  // 50+100+40+120
    assert(gov->conversation_history.turn_count == 2);
    assert(gov->current_kv_pos == 140);  // 60+80
    
    // Verify summary in memory
    ethervox_memory_search_result_t* results;
    uint32_t count;
    const char* tags[] = {"context_summary"};
    ethervox_memory_search(memory, NULL, tags, 1, 10, &results, &count);
    
    assert(count == 1);
    assert(results[0].entry.importance >= 0.9);
    assert(strstr(results[0].entry.text, "Python") != NULL);
    
    cleanup_test_memory(memory);
    cleanup_test_governor(gov);
}
```

---

### Integration Tests

**File**: `tests/integration/test_context_overflow_integration.c`

#### Test 5: Seamless Overflow Handling

```c
void test_seamless_overflow() {
    ethervox_governor_t* gov = create_governor_with_model();
    ethervox_memory_store_t* memory = init_memory();
    
    // Register context_manage tool
    register_context_tools(gov, memory);
    
    // Have long conversation (simulate 7000 tokens used)
    for (int i = 0; i < 30; i++) {
        char query[256];
        snprintf(query, sizeof(query), "Question %d about topic", i);
        
        char* response = NULL;
        char* error = NULL;
        
        ethervox_governor_status_t status = ethervox_governor_execute(
            gov, query, &response, &error, NULL, NULL, NULL, NULL
        );
        
        assert(status == ETHERVOX_GOVERNOR_SUCCESS);
        assert(error == NULL);
        
        free(response);
    }
    
    // Verify context was managed automatically
    assert(gov->context_manager.overflow_event_count >= 1);
    
    // Verify summaries were created
    ethervox_memory_search_result_t* results;
    uint32_t count;
    const char* tags[] = {"context_summary"};
    ethervox_memory_search(memory, NULL, tags, 1, 10, &results, &count);
    
    assert(count >= 1);  // At least one summary created
    
    // Verify conversation still works
    char* final_response = NULL;
    ethervox_governor_execute(
        gov, "Summarize everything we discussed",
        &final_response, NULL, NULL, NULL, NULL, NULL
    );
    
    assert(final_response != NULL);
    assert(strlen(final_response) > 0);
    
    free(final_response);
    cleanup_memory(memory);
    cleanup_governor(gov);
}
```

#### Test 6: Multiple Overflow Events

```c
void test_multiple_overflows() {
    // Simulate very long conversation
    // Trigger overflow 3+ times
    // Verify each time:
    //   - Context managed successfully
    //   - Summary created
    //   - Conversation continues
    //   - No data loss (summaries retrievable)
}
```

#### Test 7: Emergency Fallback

```c
void test_emergency_overflow() {
    // Fill context to 96%
    // Send message that pushes to 100%
    // LLM doesn't respond with tool call (simulated)
    // Verify emergency shift_window triggered
    // Verify conversation doesn't crash
}
```

---

### Manual Testing Checklist

#### Scenario 1: Long Conversation

1. Start fresh conversation
2. Have extended dialogue (30+ turns)
3. Monitor context usage with `/context status`
4. Observe automatic management trigger
5. Verify brief notification shown
6. Continue conversation seamlessly
7. Use `/memory search tag=context_summary` to find summaries

**Expected**:
- ✅ Conversation never fails
- ✅ Summaries created automatically
- ✅ Context stays healthy (<80% after management)

#### Scenario 2: Manual Management

1. Fill context to 75%
2. Run `/context summarize`
3. Verify summary created
4. Check `/context status` shows lower usage
5. Run `/context status` to see detailed info

**Expected**:
- ✅ Summary generated on demand
- ✅ Context freed appropriately
- ✅ Status shows accurate metrics

#### Scenario 3: Cross-Session Retrieval

1. Have long conversation with overflow
2. Exit application
3. Restart, start new conversation
4. Use `memory_search` to find previous summaries
5. Verify old context retrievable

**Expected**:
- ✅ Summaries persist across sessions
- ✅ Searchable and importable
- ✅ High importance retained

---

## Success Criteria

### Must Have (MVP)

- ✅ Context health monitoring functional
- ✅ Automatic trigger at 80% threshold
- ✅ `summarize_old` action works correctly
- ✅ Summaries stored in memory with high importance
- ✅ KV cache manipulation doesn't corrupt state
- ✅ Conversations can exceed 8K tokens without failure
- ✅ All unit tests pass
- ✅ Integration tests pass

### Should Have (V1)

- ✅ `shift_window` action implemented
- ✅ `/context status` command
- ✅ `/context summarize` manual trigger
- ✅ Brief user notifications
- ✅ Configuration options (enable/disable, thresholds)
- ✅ Emergency overflow fallback

### Nice to Have (Future)

- ⭕ `prune_unimportant` action (complex KV manipulation)
- ⭕ Importance ML prediction
- ⭕ Hierarchical summaries
- ⭕ Adaptive thresholds based on usage patterns
- ⭕ Context usage visualization

---

## Performance Targets

### Latency

- **Health Check**: <0.1ms (negligible overhead)
- **Turn Tracking**: <0.5ms per turn (minimal impact)
- **Shift Window**: <50ms (fast KV cache ops)
- **Summarize Old**: 2-5 seconds (LLM call for summary)

**User Experience**: Summarization happens transparently during LLM thinking time

### Memory Overhead

- **Turn Tracking**: ~200 bytes per turn
- **Manager State**: ~1KB total
- **For 100 turns**: ~20KB (acceptable)

### Context Savings

- **Target**: Free 30-50% of context when triggered
- **Shift Window**: Frees ~40% (keeps last 10 of 30 turns)
- **Summarize Old**: Frees ~50% (20 turns → 500 token summary)

---

## Risks & Mitigations

### Risk 1: KV Cache Corruption

**Problem**: Incorrect KV manipulation could corrupt model state

**Mitigation**:
- Extensive unit testing of KV operations
- Validate KV positions after every manipulation
- Add assertion checks in debug builds
- Fallback to safe shift if corruption detected

### Risk 2: Summary Quality

**Problem**: Generated summaries might lose critical information

**Mitigation**:
- Use detailed summary level by default
- Store original turns in memory before summarizing (optional)
- Allow user to retrieve full history if needed
- Test summary quality manually

### Risk 3: LLM Not Using Tool

**Problem**: LLM might ignore context_manage tool call request

**Mitigation**:
- Strong system prompt wording ("You MUST call...")
- Emergency fallback: force shift_window at 95%
- Monitor and log ignored warnings
- Consider fine-tuning prompt if needed

### Risk 4: Performance Degradation

**Problem**: Frequent summarization might slow down responses

**Mitigation**:
- Only trigger at 80%+ (not frequently in normal use)
- Async summarization (if feasible)
- Use "brief" summary by default
- Cache summaries to avoid re-summarizing

---

## Future Enhancements

### Enhancement 1: Adaptive Thresholds

Learn optimal trigger points based on user behavior:
- Users with short messages: Trigger at 85%
- Users with long messages: Trigger at 75% (preemptive)

### Enhancement 2: Importance Prediction ML

Train lightweight model to predict turn importance:
- Features: Message length, contains questions, tool usage, sentiment
- Output: Importance score 0.0-1.0
- Use for better pruning decisions

### Enhancement 3: Hierarchical Summaries

Multi-level summarization:
```
Level 1 (Session): "User discussed Python, debugging, and project setup"
Level 2 (Topic): "Python: basics, lists, error handling"
Level 3 (Detail): "User had IndexError on line 42, fixed with bounds check"
```

Store in tree structure, retrieve as needed

### Enhancement 4: Cross-Session Context

Link summaries across days:
```
Day 1: "Project planning session"
Day 2: "Implementation started" [references Day 1]
Day 3: "Debugging issues" [references Day 2]
```

Build conversation graph for long-term memory

---

## Configuration

Add to `include/ethervox/config.h`:

```c
// Context Management Configuration
#ifndef ETHERVOX_CONTEXT_MANAGEMENT_ENABLED
#define ETHERVOX_CONTEXT_MANAGEMENT_ENABLED 1
#endif

#ifndef ETHERVOX_CONTEXT_WARNING_THRESHOLD
#define ETHERVOX_CONTEXT_WARNING_THRESHOLD 0.60f  // 60%
#endif

#ifndef ETHERVOX_CONTEXT_CRITICAL_THRESHOLD
#define ETHERVOX_CONTEXT_CRITICAL_THRESHOLD 0.80f  // 80%
#endif

#ifndef ETHERVOX_CONTEXT_EMERGENCY_THRESHOLD
#define ETHERVOX_CONTEXT_EMERGENCY_THRESHOLD 0.95f  // 95%
#endif

#ifndef ETHERVOX_CONTEXT_KEEP_RECENT_TURNS
#define ETHERVOX_CONTEXT_KEEP_RECENT_TURNS 10
#endif

#ifndef ETHERVOX_CONTEXT_SUMMARY_DETAIL
#define ETHERVOX_CONTEXT_SUMMARY_DETAIL "moderate"  // "brief", "moderate", "detailed"
#endif

#ifndef ETHERVOX_CONTEXT_STORE_SUMMARIES
#define ETHERVOX_CONTEXT_STORE_SUMMARIES 1
#endif
```

---

## Deliverables

### Code Files

1. `src/plugins/context_tools/context_manage.c` - Tool wrapper
2. `src/plugins/context_tools/context_actions.c` - Action implementations
3. `src/plugins/context_tools/context_utils.c` - Helper functions
4. `include/ethervox/context_tools.h` - Public API
5. Updates to `src/governor/governor.c` - Health monitoring, turn tracking
6. Updates to `src/main.c` - `/context` commands

### Tests

1. `tests/test_context_overflow.c` - Unit tests
2. `tests/integration/test_context_overflow_integration.c` - Integration tests
3. `tests/manual_test_scenarios.md` - Manual test checklist

### Documentation

1. `docs/context-overflow-management.md` - This document
2. `docs/user-guide.md` - Updated with `/context` commands
3. `README.md` - Updated feature list

---

## Timeline

**Total Duration**: 5 weeks

| Week | Focus | Deliverables |
|------|-------|--------------|
| 1 | Foundation | Health monitoring, turn tracking |
| 2 | Tool Implementation | summarize_old, shift_window actions |
| 3 | Auto-triggering | System warnings, emergency fallback |
| 4 | User Interface | Commands, status display, notifications |
| 5 | Integration & Testing | Full testing, polish, documentation |

**Milestone 1** (End Week 2): Manual tool calls work  
**Milestone 2** (End Week 3): Automatic triggering works  
**Milestone 3** (End Week 5): Production-ready

---

## Conclusion

Mid-generation context overflow management transforms a hard failure mode into a seamless user experience. By empowering the LLM to manage its own context intelligently, we:

1. **Eliminate crashes** from context exhaustion
2. **Preserve information** via summaries in memory
3. **Enable long conversations** (effectively unlimited)
4. **Match competitors** (ChatGPT, Claude) in UX
5. **Leverage existing infrastructure** (memory system)

This feature is **critical for commercial viability** and **dramatically improves user experience**.

---

**Document Version**: 1.0  
**Last Updated**: November 27, 2025  
**Status**: Ready for Implementation  
**Next Step**: Begin Phase 1 - Foundation
