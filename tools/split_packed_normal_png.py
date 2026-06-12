#!/usr/bin/env python3
"""
Split War Thunder/Dagor packed normal PNGs into a standard normal map and ARM map.

Matches DagorAssetsExtractorApp's packed normal split logic:
  source R = smoothness source
  source G = normal Y
  source B = insulator/metal source
  source A = normal X

Outputs:
  normal RGB = normal X, normal Y, reconstructed normal Z
  ARM RGB    = ambient occlusion, roughness, metallic
             = 255, 255 - source R, 255 - source B
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path
from typing import Iterable

try:
    from PIL import Image
except ImportError as exc:
    raise SystemExit(
        "Pillow is required. Install it with: python -m pip install pillow"
    ) from exc


def normalize_texture_semantic_name(value: str) -> str:
    name = Path(value).stem
    normalized: list[str] = []
    previous_underscore = False

    for char in name:
        if char.isalnum():
            normalized.append(char.lower())
            previous_underscore = False
        elif not previous_underscore:
            normalized.append("_")
            previous_underscore = True

    while normalized and normalized[0] == "_":
        normalized.pop(0)
    while normalized and normalized[-1] == "_":
        normalized.pop()

    return "".join(normalized)


def looks_like_packed_normal_map(path: Path) -> bool:
    normalized = normalize_texture_semantic_name(path.name)
    return normalized.endswith("_n") or "_n_" in normalized


def build_output_path(
    source: Path,
    output_dir: Path | None,
    suffix: str,
    extension: str = ".png",
) -> Path:
    directory = output_dir if output_dir is not None else source.parent
    return directory / f"{source.stem}{suffix}{extension}"


def iter_png_inputs(paths: Iterable[Path], recursive: bool) -> Iterable[Path]:
    for path in paths:
        if path.is_dir():
            pattern = "**/*.png" if recursive else "*.png"
            yield from sorted(path.glob(pattern))
        else:
            yield path


def split_packed_normal_png(
    source: Path,
    normal_output: Path,
    arm_output: Path,
    overwrite: bool,
) -> None:
    if source.suffix.lower() != ".png":
        raise ValueError(f"Only PNG input is supported: {source}")

    if not source.is_file():
        raise FileNotFoundError(source)

    if not overwrite:
        for output in (normal_output, arm_output):
            if output.exists():
                raise FileExistsError(f"Output already exists: {output}")

    with Image.open(source) as input_image:
        if "A" not in input_image.getbands():
            raise ValueError(
                f"{source} has no alpha channel. Dagor packed normals store normal X in alpha."
            )

        image = input_image.convert("RGBA")

    normal = Image.new("RGBA", image.size)
    arm = Image.new("RGBA", image.size)

    normal_pixels = bytearray()
    arm_pixels = bytearray()

    data = image.tobytes()
    for i in range(0, len(data), 4):
        src_r = data[i + 0]
        src_g = data[i + 1]
        src_b = data[i + 2]
        src_a = data[i + 3]

        x = (src_a / 255.0) * 2.0 - 1.0
        y = (src_g / 255.0) * 2.0 - 1.0
        z_sq = 1.0 - x * x - y * y
        z = math.sqrt(z_sq) if z_sq > 0.0 else 0.0
        normal_z = max(0, min(255, int(round((z + 1.0) * 0.5 * 255.0))))

        normal_pixels.extend((src_a, src_g, normal_z, 255))
        arm_pixels.extend((255, 255 - src_r, 255 - src_b, 255))

    normal.frombytes(bytes(normal_pixels))
    arm.frombytes(bytes(arm_pixels))

    normal_output.parent.mkdir(parents=True, exist_ok=True)
    arm_output.parent.mkdir(parents=True, exist_ok=True)

    normal.save(normal_output, "PNG")
    arm.save(arm_output, "PNG")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Split Dagor/War Thunder packed normal PNGs into standalone normal and ARM PNGs."
        )
    )
    parser.add_argument(
        "inputs",
        nargs="+",
        type=Path,
        help="PNG files or directories containing PNG files.",
    )
    parser.add_argument(
        "-o",
        "--output-dir",
        type=Path,
        help="Directory for generated files. Defaults to each input file's directory.",
    )
    parser.add_argument(
        "--recursive",
        action="store_true",
        help="When an input is a directory, process PNGs recursively.",
    )
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="Replace existing generated output files.",
    )
    parser.add_argument(
        "--require-normal-name",
        action="store_true",
        help="Only process files whose names look like Dagor packed normals, such as *_n.png.",
    )
    parser.add_argument(
        "--normal-suffix",
        default="_normal",
        help="Suffix for generated normal maps. Default: _normal",
    )
    parser.add_argument(
        "--arm-suffix",
        default="_arm",
        help="Suffix for generated ARM maps. Default: _arm",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    inputs = list(iter_png_inputs(args.inputs, args.recursive))
    if not inputs:
        print("No PNG inputs found.", file=sys.stderr)
        return 1

    failures = 0
    for source in inputs:
        if args.require_normal_name and not looks_like_packed_normal_map(source):
            print(f"skip: {source} (name does not look like a packed normal map)")
            continue

        normal_output = build_output_path(source, args.output_dir, args.normal_suffix)
        arm_output = build_output_path(source, args.output_dir, args.arm_suffix)

        try:
            split_packed_normal_png(
                source=source,
                normal_output=normal_output,
                arm_output=arm_output,
                overwrite=args.overwrite,
            )
        except Exception as exc:
            failures += 1
            print(f"error: {source}: {exc}", file=sys.stderr)
            continue

        print(f"ok: {source}")
        print(f"  normal: {normal_output}")
        print(f"  arm:    {arm_output}")

    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
