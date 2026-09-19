# Codex pet faces for Gemini Live

`convert_codex_pet.py` turns a Codex-compatible pet package into six animation
strips for StackChan's **320 × 240 landscape display**. Run it with `uv`; its
Pillow dependency is pinned in the script. No Node installer is needed.

From the repository root:

```sh
uv run firmware/tools/convert_codex_pet.py \
  https://www.codexpets.app/pets/github-com-isdou-douos \
  --preview /tmp/gemini-pet-preview.png
```

Local packages work offline:

```sh
uv run firmware/tools/convert_codex_pet.py ~/Downloads/my-pet.zip
uv run firmware/tools/convert_codex_pet.py ~/.codex/pets/my-pet/
uv run firmware/tools/convert_codex_pet.py ~/.codex/pets/my-pet/pet.json
```

An HTTPS ZIP URL is also accepted. Downloads are bounded; ZIPs are read without
extracting paths or executing anything. The manifest must name a relative
`spritesheetPath`.

## Display and animation contract

| Gemini state | Pet move | Sprite row | Frames |
| --- | --- | --- | --- |
| Listening / recording | Wave | 3 | 4 |
| Error | Failed | 5 | 8 |
| Speaking / playback | Review | 8 | 6 |
| Thinking | Run (processing) | 7 | 6 |
| Ready for the next turn | Waiting | 6 | 6 |
| Connecting | Idle | 0 | 6 |

The converter uses OpenAI's [Codex pet contract](https://github.com/openai/skills/blob/main/skills/.curated/hatch-pet/references/codex-pet-contract.md)
and [per-frame animation timings](https://github.com/openai/skills/blob/main/skills/.curated/hatch-pet/references/animation-rows.md),
including the longer final-frame holds. Empty trailing columns are not exported.

Supported atlases:

- Version 1 (default): 1536 × 1872, eight columns and nine rows.
- Version 2 (`spriteVersionNumber: 2`): 1536 × 2288, eight columns and eleven rows.
  The two additional look-direction rows are not needed by Gemini.
- Cells are always 192 × 208. The atlas must be a static PNG or WebP.

Frames are stored at **160 × 174** to fit all six moves in flash. LVGL scales
them proportionally into the same **176 × 192 display area**, centered on the
320 × 240 screen with room for the status bar and home control. All frames use
the same scale and padding: no stretching or per-frame cropping that would make
the character jitter. Transparency is composited over `#102A43` before conversion
to little-endian RGB565.

Use `--width`, `--height`, and `--background` to adjust the stored canvas. Height
is limited to 208 pixels, and total exported assets are limited to 2 MiB.
`--preview` writes a contact sheet with Idle, Wave, Failed, Review, Waiting, and
Run rows.

## Output and build

Default output: `firmware/main/assets/assets_bin/gemini_pet/`.

- `gemini_pet.json`: identity, source URL, source image SHA-256, dimensions,
  background, filenames, and frame durations.
- `gemini_pet_idle.bin`, `gemini_pet_wave.bin`, `gemini_pet_failed.bin`,
  `gemini_pet_review.bin`, `gemini_pet_waiting.bin`, `gemini_pet_run.bin`:
  LVGL v9 image files, with each move's frames stacked
  vertically. The 12-byte header is `<BBHHHHH`: magic `0x19`, RGB565 format
  `0x12`, flags `0`, width, strip height, row stride, reserved `0`.

`CodexPetAvatar` slices the memory-mapped strips into immutable frame descriptors.
It does not decode WebP/PNG or fetch network assets while talking; LVGL draws
preconverted frames with a fixed scale. Whole-character animations replace the
default eye/mouth modifiers.
Shake still resets the conversation and gives the pet a brief on-screen wobble;
no servo movement is needed for that feedback. The source pet's Jump animation
is not currently exported or used by the app.

The included DouOS export is about 1.91 MiB. The combined assets partition image
is about 3.83 MiB, within the existing 4 MiB partition. Generated assets are kept
in the repository, so normal firmware builds remain offline and do not require
Pillow. Regenerating any pet asset triggers repacking on the next build.

After conversion, build and flash **both firmware and assets** from `firmware/`
with the ESP-IDF environment activated:

```sh
idf.py build
idf.py flash
```

Flashing only the application partition will not install a new pet. Invalid or
missing pet assets produce a visible reflash message instead of dereferencing
invalid image data. Other apps continue to use the existing default avatar.

## Tests

```sh
uv run firmware/tools/test_convert_codex_pet.py
cmake -S firmware/tests -B firmware/build-host-tests
cmake --build firmware/build-host-tests
ctest --test-dir firmware/build-host-tests --output-on-failure
```

Converter tests cover row/frame selection, timings, RGB565 headers and byte
order, alpha compositing, stable scaling, reproducibility, v2 ZIPs, input
validation, path traversal, and display/flash limits. Host firmware tests cover
animation boundaries, loop timing, delayed updates, and clock wraparound.

## Artwork attribution

The included **DouOS / XiaoDou** artwork is by [isdou](https://github.com/isdou),
from [its Codex Pets gallery page](https://www.codexpets.app/pets/github-com-isdou-douos).
The downloaded package contains `pet.json` and `spritesheet.webp`, but no explicit
artwork license. This repository's code license does not grant rights to that
third-party artwork; confirm the creator's terms before redistributing it.
The generated manifest preserves the source URL and image hash for provenance.
