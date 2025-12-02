# LLM Test Report Format

## Report Filename

```
llm_test_report_<unix_timestamp>.log
```

Example: `llm_test_report_1704067200.log`

## Report Structure

### Header Section

```
EthervoxAI LLM Tool Usage Test Report
======================================

Timestamp: Mon Jan 1 12:00:00 2024
Model Path: models/granite-4.0-h-tiny-Q4_K_M.gguf
```

### Test Sections

Each test includes:
- Test header (e.g., "=== Test 1: Memory Add Tool ===")
- Test execution details
- Pass/fail status
- Informational messages

Example:
```
=== Test 1: Memory Add Tool ===
Query: "Remember that the capital of France is Paris"
✓ Memory add tool executed
✓ Response contains tool call
✓ Tool call successful
✓ Test passed: LLM correctly used memory_store_add tool

=== Test 2: Memory Search Tool ===
Query: "What is the capital of France?"
✓ Memory search executed
✓ Response retrieved "Paris"
✓ Test passed: LLM correctly used memory_search tool
```

### Summary Section

```
=== Test Summary ===
Tests Passed:  8
Tests Failed:  0
Tests Skipped: 0
Total Tests:   8
Pass Rate:     100.0%
Duration:      312 seconds
```

## Crash Report Format

### Crash Report Filename

```
llm_test_crash_<unix_timestamp>.log
```

Example: `llm_test_crash_1704067250.log`

### Crash Report Content

```
ETHERVOXAI LLM SUBSYSTEM CRASH REPORT
=====================================

Timestamp: Mon Jan 1 12:00:50 2024

Signal: SIGSEGV (Segmentation fault)

Context: Test 8: Long Runtime Stress Test
Query Number: 15 of 30

Details:
The LLM subsystem encountered a crash during execution.
This crash was caught by the crash detection system and
the test suite was able to recover and continue.

Recovery: SUCCESS - Test suite continued after crash

Recommendations:
- Check model file integrity
- Review recent LLM subsystem changes
- Verify memory management in tool execution
- Run stress test again to check for reproducibility
- Check system resources (memory, disk space)

Debug Information:
- Governor instance: Valid
- Model loaded: Yes
- Tools registered: Yes
- Memory store: Valid

End of crash report
```

## Status Indicators

### Console Output

✓ - Test passed (green)
❌ - Test failed (red)
⚠ - Warning or partial success (yellow)
ℹ - Informational message (cyan)
🎯 - All tests passed (green, bold)
💡 - Suggestion or tip (yellow)

### File Output

```
✓ Test passed: <description>
❌ Test failed: <description>
ℹ Info: <message>
```

## Sample Complete Report

```
EthervoxAI LLM Tool Usage Test Report
======================================

Timestamp: Mon Jan 1 12:00:00 2024
Model Path: models/granite-4.0-h-tiny-Q4_K_M.gguf

=== Initialization ===
ℹ Using auto-loaded model
✓ Model ready

=== Test 1: Memory Add Tool ===
Query: "Remember that the capital of France is Paris"
✓ Memory add tool executed successfully
✓ Test passed

=== Test 2: Memory Search Tool ===
Query: "What is the capital of France?"
✓ Memory search executed successfully
✓ Response contains "Paris"
✓ Test passed

=== Test 3: Calculator Tool ===
Query: "What is 789 * 456?"
✓ Calculator tool invoked
✓ Result: 359784
✓ Test passed

=== Test 4: Memory Correction (Adaptive) ===
Query: "Actually, I made a mistake. The color is red, not blue"
✓ Correction tool invoked
✓ Memory updated successfully
✓ Test passed

=== Test 5: Tag-Based Memory Search ===
Query: "Find all memories tagged with 'important'"
✓ Tag search executed
✓ Results retrieved
✓ Test passed

=== Test 6: Multi-Tool Orchestration ===
Query: "Calculate 150 + 75 and remember the result as 'total cost'"
✓ Calculator invoked: 225
✓ Memory add invoked: stored "total cost = 225"
✓ Multi-tool coordination successful
✓ Test passed

=== Test 7: Model Load/Unload Lifecycle ===
✓ Governor created
✓ Model loaded: models/granite-4.0-h-tiny-Q4_K_M.gguf
✓ Inference successful
✓ Model unloaded
✓ Governor cleaned up
✓ Test passed

=== Test 8: Long Runtime Stress Test ===
Duration: 300 seconds (5 minutes)
Query interval: 10 seconds
Total queries: 30

Progress updates:
[30s] Completed 3/30 queries, success rate: 100.0%
[60s] Completed 6/30 queries, success rate: 100.0%
[90s] Completed 9/30 queries, success rate: 100.0%
[120s] Completed 12/30 queries, success rate: 100.0%
[150s] Completed 15/30 queries, success rate: 100.0%
[180s] Completed 18/30 queries, success rate: 100.0%
[210s] Completed 21/30 queries, success rate: 100.0%
[240s] Completed 24/30 queries, success rate: 100.0%
[270s] Completed 27/30 queries, success rate: 100.0%
[300s] Completed 30/30 queries, success rate: 100.0%

Results:
- Successful: 30
- Failed: 0
- Crashed: 0
- Success rate: 100.0%

✓ Stress test passed with >90% success rate

=== Test Summary ===
Tests Passed:  8
Tests Failed:  0
Tests Skipped: 0
Total Tests:   8
Pass Rate:     100.0%
Duration:      312 seconds

```

## Using Reports for Debugging

### High Failure Rate

Check the report for:
- Which specific tests failed
- Tool invocation patterns
- Response content (hallucinations vs tool usage)

Actions:
- Review system prompt in `src/llm/tool_registry.c`
- Verify tool descriptions are clear
- Try different model (Phi-3.5 recommended)

### Crashes Detected

Check crash reports for:
- Signal type (SIGSEGV = memory issue, SIGABRT = assertion failure)
- Which test was running
- Query number in stress test

Actions:
- Run valgrind to detect memory issues
- Check recent code changes in failing area
- Verify model file integrity
- Test with minimal configuration

### Intermittent Failures

If stress test shows variable success rate:
- Check system resources (memory, CPU)
- Look for race conditions in tool execution
- Verify thread safety of tool implementations
- Monitor memory usage during test

## Report Retention

Recommended retention policy:
- Keep last 10 test reports
- Archive crash reports indefinitely
- Delete old reports after 30 days (if no crashes)

Automation:
```bash
# Clean old test reports (keep last 10)
ls -t llm_test_report_*.log | tail -n +11 | xargs rm -f

# Archive crash reports
mkdir -p crash_reports_archive
mv llm_test_crash_*.log crash_reports_archive/ 2>/dev/null
```
