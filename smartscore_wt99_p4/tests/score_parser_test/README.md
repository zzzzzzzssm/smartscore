# Compact score host tests

These tests compile the production `compact_score.c` and
`dashscope_omr_response.c` sources with ESP-IDF's cJSON source. They do not
contain or require an API key.

Run on Windows with Visual Studio Build Tools installed:

```powershell
powershell -ExecutionPolicy Bypass -File tests/score_parser_test/run_tests.ps1
```

The suite covers numbered notation, a two-staff piano score, deterministic
sorting, measure and schema errors, both JSON response layers,
`finish_reason=length`, usage parsing, and HTTP retry policy.
