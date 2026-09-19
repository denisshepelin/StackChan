# /// script
# requires-python = ">=3.11"
# dependencies = ["pillow==12.3.0"]
# ///
"""Convert a Codex pet folder, ZIP, or gallery URL to Gemini's LVGL RGB565 assets."""

from __future__ import annotations

import argparse
from array import array
from dataclasses import dataclass
import hashlib
from html.parser import HTMLParser
from io import BytesIO
import json
from pathlib import Path, PurePosixPath
import struct
import sys
import tempfile
from urllib.parse import urlparse
from urllib.request import Request, urlopen
import zipfile

from PIL import Image

MAX_SOURCE_BYTES = 16 * 1024 * 1024
MAX_ASSET_BYTES = 2 * 1024 * 1024
CELL_WIDTH, CELL_HEIGHT = 192, 208
LVGL_HEADER = struct.Struct("<BBHHHHH")
DEFAULT_OUTPUT = Path(__file__).resolve().parents[1] / "main/assets/assets_bin/gemini_pet"


@dataclass(frozen=True)
class CodexPetMove:
    """A standard Codex sprite row and its ordered frame durations in milliseconds."""

    name: str
    row: int
    durations_ms: tuple[int, ...]


CODEX_PET_MOVES = (
    CodexPetMove("idle", 0, (280, 110, 110, 140, 140, 320)),
    CodexPetMove("wave", 3, (140, 140, 140, 280)),
    CodexPetMove("failed", 5, (140, 140, 140, 140, 140, 140, 140, 240)),
    CodexPetMove("review", 8, (150, 150, 150, 150, 150, 280)),
    CodexPetMove("waiting", 6, (150, 150, 150, 150, 150, 260)),
    CodexPetMove("run", 7, (120, 120, 120, 120, 120, 220)),
)


class PetDownloadLinks(HTMLParser):
    """Collect ZIP download links without executing gallery scripts."""

    def __init__(self) -> None:
        super().__init__()
        self.urls: set[str] = set()

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        if tag == "a":
            href = dict(attrs).get("href", "") or ""
            if urlparse(href).scheme == "https" and urlparse(href).path.lower().endswith(".zip"):
                self.urls.add(href)


def download_pet_bytes(url: str) -> bytes:
    """Download a bounded HTTPS resource; never run package installers or scripts."""
    if urlparse(url).scheme != "https":
        raise ValueError("Codex pet download requires HTTPS")
    request = Request(url, headers={"User-Agent": "StackChan-Codex-Pet-Converter/1"})
    with urlopen(request, timeout=30) as response:
        if urlparse(response.url).scheme != "https":
            raise ValueError("Codex pet download redirected away from HTTPS")
        content = response.read(MAX_SOURCE_BYTES + 1)
    if len(content) > MAX_SOURCE_BYTES:
        raise ValueError("Codex pet download exceeds 16 MiB")
    return content


def validate_pet_path(value: object) -> PurePosixPath:
    """Reject absolute or escaping sprite paths before reading any package resource."""
    if not isinstance(value, str) or not value or "\\" in value or ":" in value:
        raise ValueError("Codex pet spritesheetPath must be a relative package path")
    path = PurePosixPath(value)
    if path.is_absolute() or ".." in path.parts or path == PurePosixPath("."):
        raise ValueError("Codex pet spritesheetPath escapes the package")
    return path


def decode_pet_manifest(content: bytes) -> dict:
    """Validate the Codex pet identity and relative sprite-sheet reference."""
    if len(content) > 65536:
        raise ValueError("Codex pet manifest exceeds 64 KiB")
    manifest = json.loads(content)
    if not isinstance(manifest, dict):
        raise ValueError("Codex pet manifest must be a JSON object")
    for key in ("id", "displayName"):
        if not isinstance(manifest.get(key), str) or not manifest[key].strip():
            raise ValueError(f"Codex pet manifest requires {key}")
    validate_pet_path(manifest.get("spritesheetPath"))
    version = manifest.get("spriteVersionNumber", 1)
    if type(version) is not int or version not in (1, 2):
        raise ValueError("Codex pet spriteVersionNumber must be 1 or 2")
    return manifest


def read_pet_zip(content: bytes) -> tuple[dict, bytes]:
    """Read only the manifest and sprite from a ZIP, without extracting archive paths."""
    with zipfile.ZipFile(BytesIO(content)) as archive:
        entries = archive.infolist()
        names = [entry.filename for entry in entries]
        if len(names) != len(set(names)):
            raise ValueError("Codex pet ZIP contains duplicate entries")
        manifests = [name for name in names if PurePosixPath(name).name == "pet.json"]
        if len(manifests) != 1:
            raise ValueError("Codex pet ZIP must contain exactly one pet.json")
        manifest_path = validate_pet_path(manifests[0])
        if archive.getinfo(str(manifest_path)).file_size > 65536:
            raise ValueError("Codex pet manifest exceeds 64 KiB")
        manifest = decode_pet_manifest(archive.read(str(manifest_path)))
        sprite_path = manifest_path.parent / validate_pet_path(manifest["spritesheetPath"])
        if archive.getinfo(str(sprite_path)).file_size > MAX_SOURCE_BYTES:
            raise ValueError("Codex pet sprite exceeds 16 MiB")
        return manifest, archive.read(str(sprite_path))


def load_codex_pet(source: str) -> tuple[dict, bytes]:
    """Load a local pet package or an HTTPS ZIP/gallery link with bounded image input."""
    if urlparse(source).scheme in ("https", "http"):
        content = download_pet_bytes(source)
        if zipfile.is_zipfile(BytesIO(content)):
            return read_pet_zip(content)
        url = urlparse(source)
        if url.hostname not in ("codexpets.app", "www.codexpets.app") or not url.path.startswith("/pets/"):
            raise ValueError("Codex pet URL must point to a ZIP or a codexpets.app pet page")
        links = PetDownloadLinks()
        links.feed(content.decode("utf-8"))
        if len(links.urls) != 1:
            raise ValueError("Codex pet gallery page must contain exactly one ZIP download")
        return read_pet_zip(download_pet_bytes(next(iter(links.urls))))

    path = Path(source).expanduser()
    if path.is_dir():
        path /= "pet.json"
    if path.name != "pet.json":
        if path.stat().st_size > MAX_SOURCE_BYTES:
            raise ValueError("Codex pet ZIP exceeds 16 MiB")
        return read_pet_zip(path.read_bytes())
    if path.stat().st_size > 65536:
        raise ValueError("Codex pet manifest exceeds 64 KiB")
    manifest = decode_pet_manifest(path.read_bytes())
    root = path.parent.resolve()
    sprite = (root / str(validate_pet_path(manifest["spritesheetPath"]))).resolve()
    if not sprite.is_relative_to(root):
        raise ValueError("Codex pet sprite symlink escapes the package")
    if sprite.stat().st_size > MAX_SOURCE_BYTES:
        raise ValueError("Codex pet sprite exceeds 16 MiB")
    return manifest, sprite.read_bytes()


def encode_rgb565(image: Image.Image) -> bytes:
    """Encode RGB pixels as little-endian RGB565 for the CoreS3's LVGL renderer."""
    pixels = image.convert("RGB").tobytes()
    values = array("H", (
        ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        for r, g, b in zip(pixels[0::3], pixels[1::3], pixels[2::3])
    ))
    if sys.byteorder != "little":
        values.byteswap()
    return values.tobytes()


def convert_codex_pet(
    source: str,
    output: Path,
    *,
    width: int = 160,
    height: int = 174,
    background: int = 0x102A43,
    preview: Path | None = None,
    source_url: str | None = None,
) -> dict:
    """Write Gemini's vertically stacked LVGL v9 frame strips and a timed animation manifest."""
    if not (1 <= width <= 320 and 1 <= height <= 208 and 0 <= background <= 0xFFFFFF):
        raise ValueError("Codex pet frame must fit the 320x208 content area; background must be RGB hex")
    frame_total = sum(len(move.durations_ms) for move in CODEX_PET_MOVES)
    if frame_total * width * height * 2 + len(CODEX_PET_MOVES) * LVGL_HEADER.size > MAX_ASSET_BYTES:
        raise ValueError("Codex pet frames exceed the 2 MiB asset budget; reduce --width or --height")
    pet, sprite_bytes = load_codex_pet(source)
    with Image.open(BytesIO(sprite_bytes)) as original:
        rows = 11 if pet.get("spriteVersionNumber", 1) == 2 else 9
        if original.format not in ("PNG", "WEBP") or getattr(original, "n_frames", 1) != 1:
            raise ValueError("Codex pet sprite must be a static PNG or WebP atlas")
        if original.size != (8 * CELL_WIDTH, rows * CELL_HEIGHT):
            raise ValueError(f"Codex pet sprite must be {8 * CELL_WIDTH}x{rows * CELL_HEIGHT}")
        atlas = original.convert("RGBA")

    scale = min(width / CELL_WIDTH, height / CELL_HEIGHT)
    fitted_size = (max(1, round(CELL_WIDTH * scale)), max(1, round(CELL_HEIGHT * scale)))
    offset = ((width - fitted_size[0]) // 2, (height - fitted_size[1]) // 2)
    rgb = ((background >> 16) & 255, (background >> 8) & 255, background & 255)
    manifest = {
        "schemaVersion": 1,
        "pet": pet,
        "sourceUrl": source_url or (source if source.startswith("https://") else None),
        "spritesheetSha256": hashlib.sha256(sprite_bytes).hexdigest(),
        "frameWidth": width,
        "frameHeight": height,
        "backgroundColor": background,
        "animations": {},
    }
    files: dict[str, bytes] = {}
    contact_sheet = Image.new("RGB", (width * 8, height * len(CODEX_PET_MOVES)), rgb) if preview else None
    for move_index, move in enumerate(CODEX_PET_MOVES):
        name = f"gemini_pet_{move.name}.bin"
        data = bytearray(LVGL_HEADER.pack(0x19, 0x12, 0, width, height * len(move.durations_ms), width * 2, 0))
        for column in range(len(move.durations_ms)):
            cell = atlas.crop((column * CELL_WIDTH, move.row * CELL_HEIGHT,
                               (column + 1) * CELL_WIDTH, (move.row + 1) * CELL_HEIGHT))
            cell = cell.resize(fitted_size, Image.Resampling.LANCZOS)
            frame = Image.new("RGBA", (width, height), (*rgb, 255))
            frame.alpha_composite(cell, offset)
            data.extend(encode_rgb565(frame))
            if contact_sheet is not None:
                contact_sheet.paste(frame.convert("RGB"), (column * width, move_index * height))
        files[name] = bytes(data)
        manifest["animations"][move.name] = {"file": name, "durationsMs": list(move.durations_ms)}

    files["gemini_pet.json"] = (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode()
    if len(files["gemini_pet.json"]) > 16384:
        raise ValueError("Codex pet output manifest exceeds the firmware's 16 KiB limit")
    if sum(len(content) for content in files.values()) > MAX_ASSET_BYTES:
        raise ValueError("Codex pet output exceeds the 2 MiB asset budget")
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".codex-pet-", dir=output) as staging:
        for name, content in files.items():
            staged = Path(staging) / name
            staged.write_bytes(content)
        for name in files:
            (Path(staging) / name).replace(output / name)
    if preview and contact_sheet is not None:
        preview.parent.mkdir(parents=True, exist_ok=True)
        contact_sheet.save(preview)
    return manifest


def main() -> None:
    """Run the offline-capable Codex pet converter; generation never runs during firmware builds."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", help="Pet directory, pet.json, ZIP, HTTPS ZIP, or codexpets.app pet URL")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--width", type=int, default=160, help="Stored frame width; displayed in a 176x192 viewport")
    parser.add_argument("--height", type=int, default=174, help="Stored frame height; maximum 208")
    parser.add_argument("--background", default="102A43", help="Six-digit RGB background hex")
    parser.add_argument("--preview", type=Path, help="Optional PNG contact sheet: Idle, Wave, Failed, Review, Waiting, Run")
    parser.add_argument("--source-url", help="Original gallery/source URL when converting a local package")
    args = parser.parse_args()
    try:
        manifest = convert_codex_pet(args.source, args.output, width=args.width, height=args.height,
                                     background=int(args.background.removeprefix("#"), 16),
                                     preview=args.preview, source_url=args.source_url)
    except (ValueError, OSError, KeyError, zipfile.BadZipFile) as error:
        parser.exit(1, f"Codex pet conversion failed: {error}\n")
    size = sum(path.stat().st_size for path in args.output.glob("gemini_pet*"))
    print(f"Converted {manifest['pet']['displayName']}: {args.width}x{args.height} frames, {size:,} bytes")
    print(f"Assets: {args.output}")


if __name__ == "__main__":
    main()
