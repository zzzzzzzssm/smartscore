# DeepSeek V4 Flash practice-advice design

## Goal and scope

Replace the practice-advice model path with DeepSeek `deepseek-v4-flash` in
non-thinking mode. Local scoring remains the only source of scores. The model
receives a bounded, device-produced summary of the last completed practice
session and returns advice only; it must not calculate, revise, or return an AI
score.

The existing **AI 练习建议** button remains. Its dialog shows the advice
summary, prioritized practice tasks, next-session plan, and encouragement. It
does not repeat local scores. Score recognition remains on DashScope and is
outside this change. Scoring formulas, score-page presentation, MIDI/audio
capture, and unrelated voice features are also unchanged.

## Architecture

Add a dedicated `deepseek_advice` ESP-IDF component instead of extending the
legacy `doubao_client`. This keeps practice advice independent from the
DashScope OMR client and from obsolete Doubao code and credentials.

`device_api` continues to own `POST /api/ai/score`. It copies the last complete
scoring result, passes it to `deepseek_advice`, validates the returned advice,
and sends the validated JSON to the mini program. The endpoint never changes
the stored scoring result.

The client uses:

- URL: `https://api.deepseek.com/chat/completions`
- model: `deepseek-v4-flash`
- thinking: `{ "type": "disabled" }`
- response format: `{ "type": "json_object" }`
- temperature: `0.2`
- maximum output: `1500` tokens
- streaming: disabled
- credential: build-time `DEEPSEEK_API_KEY`, separate from
  `DASHSCOPE_API_KEY`

The API key is read from the build environment and injected as a private
compile definition. It is never stored in source, project defaults, mini
program storage, logs, or API responses.

## Local advice input

The client parses the scoring service's existing result instead of sending the
entire raw result. It builds a versioned advice input containing:

- piece title and locally known practice BPM;
- the final local pitch, rhythm, continuity/fluency, and overall values;
- aggregate counts such as attempted, wrong pitch, missing, extra, retry, and
  uncertain notes;
- at most 40 locally detected non-correct detail records.

Each issue carries only facts available in the scoring result, such as target
note index, issue type, expected/played MIDI note, and timing offset. Bar and
beat numbers are included only if the local scorer provides them in the
future. They are never inferred or invented. Correct note events are omitted.
When more than 40 issues exist, the builder retains aggregate counts and uses
a deterministic priority: combined pitch/rhythm error, missing note, pitch
error, rhythm error, extra/retry, then uncertain input. Within one class,
larger absolute pitch or timing errors come first and original order breaks
ties. Early/late counts and labels are derived only from a locally available
signed aligned-start offset on an event already classified as a rhythm error.

The input maps the existing local `fluency_score` to advice dimension
`continuity`; this is a label mapping only and does not alter the score. A
locally unavailable fact, including an original target BPM distinct from the
prepared practice BPM, is omitted so the model can report it through
`insufficient_data`.

## Prompt and output contract

The system and user prompts explicitly require strict JSON and state that all
scores and issue facts come from the device. The model may explain them but
must not question, recompute, replace, or add scores. It may cite only supplied
issue positions and statistics and must not infer problems from the piece
title.

The response contract is:

```json
{
  "v": 1,
  "summary": "string",
  "focus": [
    {
      "rank": 1,
      "dimension_id": "pitch|rhythm|continuity",
      "problem": "string",
      "evidence": ["string"],
      "practice": {
        "action": "string",
        "bpm": 60,
        "minutes": 5,
        "repetitions": 4,
        "target": "string"
      }
    }
  ],
  "next_session": {
    "total_minutes": 15,
    "steps": [
      { "order": 1, "action": "string", "minutes": 5 }
    ]
  },
  "encouragement": "string",
  "insufficient_data": []
}
```

`focus` contains one to three entries with ranks `1..N`. Evidence contains one
to five non-empty strings. Practice BPM is `20..400`, minutes are `1..60`, and
repetitions are `1..100`. `next_session` contains one to ten steps, each lasting
`1..60` minutes, and `total_minutes` is `1..120`. No `ai_total_score`, dimension
score, or other model-generated score is accepted or shown.

## Response handling and errors

The DeepSeek client performs two parsing layers: first the Chat Completions
envelope, then `choices[0].message.content`. It accepts a response only when
HTTP succeeded, `finish_reason` is exactly `stop`, content is non-empty, the
content parses as one JSON object, and every required field has the expected
type and bounds.

Validation rejects malformed or partial advice, out-of-range array or number
values, invalid dimension identifiers or ranks, empty required strings,
missing practice targets, and any AI score fields. The device returns a 502 error for
transport, completion, JSON, or schema failures and never exposes partial
advice. A missing key returns 503 with a DeepSeek-specific error code.

Network or model failure affects only the optional advice request. The last
local result and all normal scoring functions remain available. Advice caching
is not added in this migration; it can be added later without changing the
response schema.

## Mini-program presentation

Update the existing practice-page handler to consume the new structure. The
modal displays, in order:

1. `summary`;
2. each focus problem and its action, BPM, minutes, repetitions, evidence, and
   target;
3. the ordered `next_session.steps` and total time;
4. `encouragement` and any `insufficient_data` notices.

The modal does not display a local or AI score. The practice result page
continues to display the already-computed local scores. Existing checks and
error copy are updated from Doubao/`VEI_API_KEY` to
DeepSeek/`DEEPSEEK_API_KEY`.

## Verification

- Unit-test or host-test input compaction with correct, pitch, rhythm, missing,
  extra, and combined-error detail records, including the 40-issue limit.
- Test completion parsing for valid JSON, non-`stop` finish reasons, empty
  content, invalid inner JSON, missing fields, invalid bounds, and prohibited
  score fields.
- Test mini-program formatting with one and three focus entries, missing
  optional data, and malformed response data.
- Run the existing mini-program test suite and syntax checks.
- Build the ESP-IDF application when the configured toolchain is available;
  otherwise run component/static checks and report the build limitation.
- Verify the compiled request contains only `deepseek-v4-flash`, disabled
  thinking, JSON-object response mode, and no real credential in tracked
  files.

No live paid API request is required for completion unless explicitly
requested.
