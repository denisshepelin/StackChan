# /// script
# requires-python = ">=3.11"
# dependencies = ["pillow==12.3.0"]
# ///
"""Run with uv run firmware/tools/test_convert_codex_pet.py."""

from io import BytesIO
import json
from pathlib import Path
import struct
import tempfile
import unittest
import warnings
import zipfile

from PIL import Image, ImageDraw

from convert_codex_pet import (
    CODEX_PET_MOVES, LVGL_HEADER, PetDownloadLinks, convert_codex_pet,
    decode_pet_manifest, encode_rgb565, read_pet_zip,
)


def make_pet_manifest(**overrides):
    return {"id": "test-pet", "displayName": "Test Pet", "spritesheetPath": "spritesheet.png", **overrides}


def make_sprite_bytes(rows=9):
    image = Image.new("RGBA", (1536, rows * 208))
    draw = ImageDraw.Draw(image)
    for move in CODEX_PET_MOVES:
        for column in range(len(move.durations_ms)):
            draw.rectangle((column * 192, move.row * 208, (column + 1) * 192 - 1, (move.row + 1) * 208 - 1),
                           fill=(move.row * 20, column * 25, 100, 255))
    output = BytesIO()
    image.save(output, "PNG")
    return output.getvalue()


class CodexPetConverterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sprite = make_sprite_bytes()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.pet = self.root / "pet"
        self.pet.mkdir()
        (self.pet / "pet.json").write_text(json.dumps(make_pet_manifest()))
        (self.pet / "spritesheet.png").write_bytes(self.sprite)
        self.output = self.root / "converted"

    def test_rows_counts_frame_order_and_rgb565_headers(self):
        manifest = convert_codex_pet(str(self.pet), self.output, width=96, height=104)
        self.assertEqual(len(list(self.output.iterdir())), len(CODEX_PET_MOVES) + 1)
        for move in CODEX_PET_MOVES:
            with self.subTest(move=move.name):
                count = len(move.durations_ms)
                data = (self.output / f"gemini_pet_{move.name}.bin").read_bytes()
                self.assertEqual(LVGL_HEADER.unpack(data[:12]), (0x19, 0x12, 0, 96, 104 * count, 192, 0))
                self.assertEqual(len(data), 12 + 96 * 104 * 2 * count)
                self.assertEqual(manifest["animations"][move.name]["durationsMs"], list(move.durations_ms))
                for frame in range(count):
                    expected = ((move.row * 20 >> 3) << 11) | ((frame * 25 >> 2) << 5) | (100 >> 3)
                    self.assertEqual(struct.unpack_from("<H", data, 12 + frame * 96 * 104 * 2)[0], expected)
        first = {path.name: path.read_bytes() for path in self.output.iterdir()}
        convert_codex_pet(str(self.pet), self.output, width=96, height=104)
        self.assertEqual(first, {path.name: path.read_bytes() for path in self.output.iterdir()})

    def test_thinking_and_waiting_moves_fit_default_asset_budget(self):
        moves = {move.name: move for move in CODEX_PET_MOVES}
        self.assertEqual(moves["run"].row, 7)
        self.assertEqual(moves["run"].durations_ms, (120, 120, 120, 120, 120, 220))
        self.assertEqual(moves["waiting"].row, 6)
        self.assertEqual(moves["waiting"].durations_ms, (150, 150, 150, 150, 150, 260))
        preview = self.root / "preview.png"
        manifest = convert_codex_pet(str(self.pet), self.output, preview=preview)
        self.assertEqual((manifest["frameWidth"], manifest["frameHeight"]), (160, 174))
        self.assertLessEqual(sum(path.stat().st_size for path in self.output.iterdir()), 2 * 1024 * 1024)
        with Image.open(preview) as image:
            self.assertEqual(image.size, (160 * 8, 174 * 6))
        self.assertEqual(set(manifest["animations"]), {"idle", "wave", "failed", "review", "waiting", "run"})

    def test_transparency_and_aspect_ratio_without_per_frame_cropping(self):
        image = Image.new("RGBA", (1536, 1872))
        ImageDraw.Draw(image).rectangle((80, 80, 111, 111), fill=(255, 0, 0, 128))
        image.save(self.pet / "spritesheet.png")
        convert_codex_pet(str(self.pet), self.output, width=100, height=104, background=0x0000FF)
        data = (self.output / "gemini_pet_idle.bin").read_bytes()[12:]
        self.assertEqual(struct.unpack_from("<H", data, 0)[0], 31)
        expected = (128 >> 3) << 11 | (127 >> 3)
        self.assertEqual(struct.unpack_from("<H", data, (48 * 100 + 50) * 2)[0], expected)
        self.assertEqual(struct.unpack_from("<H", data, 100 * 104 * 2)[0], 31)

    def test_nested_zip_and_version_two(self):
        zipped = self.root / "pet.zip"
        with zipfile.ZipFile(zipped, "w") as archive:
            archive.writestr("folder/pet.json", json.dumps(make_pet_manifest(spriteVersionNumber=2)))
            archive.writestr("folder/spritesheet.png", make_sprite_bytes(rows=11))
        manifest = convert_codex_pet(str(zipped), self.output, width=48, height=52)
        self.assertEqual(manifest["pet"]["spriteVersionNumber"], 2)
        self.assertEqual(set(manifest["animations"]), {move.name for move in CODEX_PET_MOVES})

    def test_bad_atlas_leaves_existing_assets_untouched(self):
        convert_codex_pet(str(self.pet), self.output, width=48, height=52)
        before = {path.name: path.read_bytes() for path in self.output.iterdir()}
        Image.new("RGBA", (100, 100)).save(self.pet / "spritesheet.png")
        with self.assertRaisesRegex(ValueError, "1536x1872"):
            convert_codex_pet(str(self.pet), self.output)
        self.assertEqual(before, {path.name: path.read_bytes() for path in self.output.iterdir()})

    def test_invalid_metadata_and_escaping_paths(self):
        for path in ("../secret.png", "/tmp/secret.png", "C:\\secret.png", "", "https://example.com/a.png"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                decode_pet_manifest(json.dumps(make_pet_manifest(spritesheetPath=path)).encode())
        for version in (0, 3, "2", True):
            with self.subTest(version=version), self.assertRaises(ValueError):
                decode_pet_manifest(json.dumps(make_pet_manifest(spriteVersionNumber=version)).encode())
        (self.pet / "spritesheet.png").unlink()
        outside = self.root / "outside.png"
        outside.write_bytes(self.sprite)
        (self.pet / "spritesheet.png").symlink_to(outside)
        with self.assertRaisesRegex(ValueError, "symlink escapes"):
            convert_codex_pet(str(self.pet), self.output)

    def test_duplicate_zip_manifest(self):
        content = BytesIO()
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            with zipfile.ZipFile(content, "w") as archive:
                archive.writestr("pet.json", json.dumps(make_pet_manifest()))
                archive.writestr("pet.json", json.dumps(make_pet_manifest()))
        with self.assertRaisesRegex(ValueError, "duplicate"):
            read_pet_zip(content.getvalue())

    def test_display_size_and_flash_budget(self):
        for width, height in ((321, 192), (176, 240), (0, 192), (320, 208)):
            with self.subTest(width=width, height=height), self.assertRaises(ValueError):
                convert_codex_pet(str(self.pet), self.output, width=width, height=height)
        self.assertFalse(self.output.exists())

    def test_rgb565_byte_order(self):
        image = Image.new("RGB", (3, 1))
        image.putdata([(255, 0, 0), (0, 255, 0), (0, 0, 255)])
        self.assertEqual(encode_rgb565(image), b"\x00\xf8\xe0\x07\x1f\x00")

    def test_gallery_download_link(self):
        parser = PetDownloadLinks()
        parser.feed('<a href="https://example.com/pet.zip?alt=media&amp;token=public">Download</a>')
        self.assertEqual(parser.urls, {"https://example.com/pet.zip?alt=media&token=public"})


if __name__ == "__main__":
    unittest.main()
