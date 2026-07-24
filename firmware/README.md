
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

`GEMINI.LIVE` connects directly to `gemini-3.1-flash-live-preview`. Put the
short-lived key in the repository root `.env` before building:

```dotenv
GOOGLE_API_KEY=your-key
```

CMake embeds the key in the firmware image and generated files under `build/`;
it is not committed. Rebuild after rotating the key.

In the app, tap the face once to open a full-duplex session; the body LEDs turn
green and microphone streaming remains active while Gemini speaks. Gemini's
server-side VAD detects turns and interruptions. Tap again to stop streaming,
turn off the LEDs, and close the Live API session completely. The next tap opens
a new session with no carried-over context. Tool calls are not configured.

### Host-side tests

The motion coordinate helpers can be tested without ESP-IDF hardware:

```bash
cmake -S tests -B build-host-tests
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure
```

### Flash

```bash
idf.py flash
```
