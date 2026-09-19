
## Build

### Fetch Dependencies

```bash
python3 ./fetch_repos.py
```

### Tool Chains

[ESP-IDF v5.5.4](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/index.html)

### Build

```bash
idf.py build
```

### Gemini Live app

`GEMINI.LIVE` connects directly to `gemini-3.8-live` (the standard Live model,
not Extended Thinking). Put the
short-lived key in the repository root `.env` before building:

```dotenv
GOOGLE_API_KEY=your-key
```

CMake embeds the key in the firmware image and generated files under `build/`;
it is not committed. Rebuild after rotating the key.

In the app, hold the face to record and stream microphone audio; the body LEDs
turn green. Release to finish the turn and show the processing pet while thinking
(amber LEDs).
Playback begins after roughly 350 ms of response audio is buffered, or sooner
if a shorter reply is complete. The microphone is off while thinking/speaking.
Presses during thinking are ignored. Pressing during speech immediately cancels
queued playback and starts recording a new turn; release submits that turn.
Head motion stops before capture begins and stays paused during recording,
thinking, and speech to avoid servo noise. Face animations continue. Position
commands during the pause are discarded, not queued; idle movement resumes when
the turn finishes or is cancelled. Servo limits, stall protection, and automatic
torque release remain active.

Turns share the same Gemini session and conversation context. Shake the robot
to discard the session and start a new conversation; the pet briefly wobbles
on screen. Shake also cancels recording, thinking, or playback. The next press
opens a fresh session. Network failures are reported explicitly; retrying after
a failure starts a new conversation (session resumption is not implemented).
Google Search grounding is enabled; custom function calls are not configured.

The session uses 16 kHz mono PCM input and 24 kHz PCM output. Microphone capture
is resampled from the CoreS3's 24 kHz codec; no acoustic echo cancellation runs.
Automatic activity detection is disabled: ordered `activityStart`, audio, and
`activityEnd` messages delimit each turn. Capture starts locally even while the
first connection is opening, with about five seconds of bounded uplink buffering.
Buffer overflow aborts the turn rather than silently dropping words. Recording
is limited to 30 seconds, and stalled responses time out after 30 seconds.

Thinking configuration is intentionally omitted because Gemini 3.8 Live does
not support it. See Google's
[Gemini 3.8 Live migration guide](https://ai.google.dev/gemini-api/docs/models/gemini-3.8-live).
After flashing, verify press/hold/release (including pressing the eyes and mouth),
follow-up context, ignored presses while thinking, interrupting speech, and shaking
during each phase. Check that the final syllable is sent before thinking completes
and that neither interrupted nor pre-shake audio can resume playback.

### Codex pet avatar

`GEMINI.LIVE` displays the DouOS Codex pet on the 320 × 240 screen: **Wave**
while listening, **Failed** on errors, **Review** while speaking, **Run** while
thinking, **Waiting** when ready for the next turn, and **Idle** while connecting.
Shake-to-reset uses a visual wobble, not the source pet's Jump move.
Servo motion remains paused throughout each
recording/thinking/speaking turn. Other apps retain the default StackChan face.

To convert another compatible pet from the repository root:

```sh
uv run firmware/tools/convert_codex_pet.py /path/to/pet.zip
```

Then rebuild and flash both the firmware and assets with `idf.py flash`.
See [the converter guide](tools/README.md) for gallery URLs, local directories,
sizing, animation timings, tests, and artwork attribution.

### Host-side tests

Motion coordinate helpers and Gemini turn transitions can be tested without
ESP-IDF hardware. After `fetch_repos.py` has fetched Smooth UI Toolkit, the same
suite also tests motion pause using real animation code and simulated servos:

```bash
cmake -S tests -B build-host-tests
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure
```

### Flash

```bash
idf.py flash
```
