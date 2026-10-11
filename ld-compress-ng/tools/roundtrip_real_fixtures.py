#!/usr/bin/env python3
"""Round-trip real LDS fixtures through selected ld-compress-ng backends."""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path


DEFAULT_FIXTURE_DIR = (
    "reference/testdata/ld-decode-testdata-ci/"
    "1cf698d2025e8515e9ef57e34adaf85a194da96a"
)
DEFAULT_BACKENDS = ("opencl", "vulkan", "metal")
SUPPORTED_BACKENDS = ("cpu", "native-fixed", "opencl", "vulkan", "metal")
CSV_COLUMNS = [
    "fixture",
    "backend",
    "input_bytes",
    "compressed_bytes",
    "ratio",
    "compress_s",
    "verify_s",
    "decompress_s",
    "source_md5",
    "decoded_md5",
    "compressed_path",
    "decoded_path",
]


@dataclass(frozen=True)
class RunConfig:
    frame_samples: str
    lpc_order: str
    lpc_precision: str
    rice_partition_order: str
    native_threads: str
    opencl_device: str | None
    vulkan_device: str | None
    metal_device: str | None


def positive_int(text: str) -> int:
    try:
        value = int(text, 10)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"not an integer: {text}") from exc
    if value <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return value


def uint_arg(name: str, minimum: int, maximum: int):
    def parse(text: str) -> str:
        try:
            value = int(text, 10)
        except ValueError as exc:
            raise argparse.ArgumentTypeError(f"{name} must be an integer") from exc
        if value < minimum or value > maximum:
            raise argparse.ArgumentTypeError(f"{name} must be {minimum}..{maximum}")
        return text

    return parse


def parse_backends(text: str) -> list[str]:
    if not text:
        raise argparse.ArgumentTypeError("backend list cannot be empty")
    backends: list[str] = []
    for item in text.split(","):
        backend = item.strip()
        if not backend:
            raise argparse.ArgumentTypeError("backend list contains an empty item")
        if backend not in SUPPORTED_BACKENDS:
            choices = ", ".join(SUPPORTED_BACKENDS)
            raise argparse.ArgumentTypeError(f"unsupported backend {backend!r}; choose from {choices}")
        if backend not in backends:
            backends.append(backend)
    return backends


def find_fixtures(root: Path, limit: int | None) -> list[Path]:
    fixtures = sorted(path for path in root.rglob("*.lds") if path.is_file())
    if limit is not None:
        fixtures = fixtures[:limit]
    return fixtures


def md5_file(path: Path) -> str:
    try:
        digest = hashlib.md5(usedforsecurity=False)
    except TypeError:
        digest = hashlib.md5()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def compressed_suffix(backend: str) -> str:
    if backend == "cpu":
        return ".ldf"
    return ".flac.ldf"


def output_paths(run_dir: Path, fixture_root: Path, fixture: Path, backend: str) -> tuple[Path, Path]:
    relative = fixture.relative_to(fixture_root)
    backend_dir = run_dir / backend
    compressed = backend_dir / relative.with_suffix(compressed_suffix(backend))
    decoded = backend_dir / relative.with_suffix(".roundtrip.lds")
    return compressed, decoded


def compress_command(
    binary: Path,
    backend: str,
    fixture: Path,
    compressed: Path,
    config: RunConfig,
) -> list[str]:
    command = [
        str(binary),
        "compress",
        "--backend",
        backend,
        "--overwrite",
    ]
    if backend != "cpu":
        if backend == "native-fixed":
            command.extend(["--threads", config.native_threads])
        else:
            command.extend(["--threads", "1"])
        command.extend([
            "--frame-samples",
            config.frame_samples,
            "--lpc-order",
            config.lpc_order,
            "--lpc-precision",
            config.lpc_precision,
            "--rice-partition-order",
            config.rice_partition_order,
        ])
    if backend == "opencl" and config.opencl_device is not None:
        command.extend(["--device", config.opencl_device])
    if backend == "vulkan" and config.vulkan_device is not None:
        command.extend(["--device", config.vulkan_device])
    if backend == "metal" and config.metal_device is not None:
        command.extend(["--device", config.metal_device])
    command.extend([str(fixture), str(compressed)])
    return command


def verify_command(binary: Path, fixture: Path, compressed: Path) -> list[str]:
    return [str(binary), "verify", "--source", str(fixture), str(compressed)]


def decompress_command(binary: Path, compressed: Path, decoded: Path) -> list[str]:
    return [str(binary), "decompress", "--overwrite", str(compressed), str(decoded)]


def run_command(command: list[str]) -> float:
    start = time.perf_counter()
    completed = subprocess.run(
        command,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    elapsed = time.perf_counter() - start
    if completed.returncode != 0:
        print("command failed:", " ".join(command), file=sys.stderr)
        if completed.stdout:
            print(completed.stdout, end="", file=sys.stderr)
        if completed.stderr:
            print(completed.stderr, end="", file=sys.stderr)
        raise RuntimeError(f"exit code {completed.returncode}")
    return elapsed


def roundtrip_fixture(
    binary: Path,
    fixture_root: Path,
    run_dir: Path,
    fixture: Path,
    backend: str,
    config: RunConfig,
) -> dict[str, str]:
    compressed, decoded = output_paths(run_dir, fixture_root, fixture, backend)
    compressed.parent.mkdir(parents=True, exist_ok=True)
    decoded.parent.mkdir(parents=True, exist_ok=True)

    compress_s = run_command(compress_command(binary, backend, fixture, compressed, config))
    verify_s = run_command(verify_command(binary, fixture, compressed))
    decompress_s = run_command(decompress_command(binary, compressed, decoded))

    source_md5 = md5_file(fixture)
    decoded_md5 = md5_file(decoded)
    input_bytes = fixture.stat().st_size
    decoded_bytes = decoded.stat().st_size
    if input_bytes != decoded_bytes or source_md5 != decoded_md5:
        raise RuntimeError(
            f"decoded output mismatch for {fixture} via {backend}: "
            f"source {input_bytes} bytes {source_md5}, decoded {decoded_bytes} bytes {decoded_md5}"
        )

    compressed_bytes = compressed.stat().st_size
    ratio = compressed_bytes / input_bytes if input_bytes else 0.0
    return {
        "fixture": fixture.relative_to(fixture_root).as_posix(),
        "backend": backend,
        "input_bytes": str(input_bytes),
        "compressed_bytes": str(compressed_bytes),
        "ratio": f"{ratio:.6f}",
        "compress_s": f"{compress_s:.3f}",
        "verify_s": f"{verify_s:.3f}",
        "decompress_s": f"{decompress_s:.3f}",
        "source_md5": source_md5,
        "decoded_md5": decoded_md5,
        "compressed_path": str(compressed),
        "decoded_path": str(decoded),
    }


def write_csv(path: Path, rows: list[dict[str, str]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=CSV_COLUMNS)
        writer.writeheader()
        writer.writerows(rows)


def format_bytes(value: int) -> str:
    return f"{value:,}"


def write_markdown(path: Path, rows: list[dict[str, str]], fixtures: list[Path], fixture_root: Path) -> None:
    fixture_names = [fixture.relative_to(fixture_root).as_posix() for fixture in fixtures]
    backends = sorted({row["backend"] for row in rows})

    with path.open("w", encoding="utf-8") as output:
        output.write("# Real Fixture Roundtrip Report\n\n")
        output.write(f"- Fixtures: `{fixture_root}`\n")
        output.write(f"- Fixture count: `{len(fixture_names)}`\n")
        output.write(f"- Backends: `{','.join(backends)}`\n")
        output.write("\n")
        output.write("## Aggregate\n\n")
        output.write("| Backend | Input bytes | Compressed bytes | Ratio | Compress s | Verify s | Decompress s |\n")
        output.write("| --- | ---: | ---: | ---: | ---: | ---: | ---: |\n")
        for backend in backends:
            backend_rows = [row for row in rows if row["backend"] == backend]
            input_bytes = sum(int(row["input_bytes"]) for row in backend_rows)
            compressed_bytes = sum(int(row["compressed_bytes"]) for row in backend_rows)
            compress_s = sum(float(row["compress_s"]) for row in backend_rows)
            verify_s = sum(float(row["verify_s"]) for row in backend_rows)
            decompress_s = sum(float(row["decompress_s"]) for row in backend_rows)
            ratio = compressed_bytes / input_bytes if input_bytes else 0.0
            output.write(
                f"| `{backend}` | {format_bytes(input_bytes)} | {format_bytes(compressed_bytes)} | "
                f"{ratio:.6f} | {compress_s:.3f} | {verify_s:.3f} | {decompress_s:.3f} |\n"
            )

        output.write("\n## Per Fixture\n\n")
        output.write("| Fixture | Backend | Input bytes | Compressed bytes | Ratio | Compress s | Verified |\n")
        output.write("| --- | --- | ---: | ---: | ---: | ---: | --- |\n")
        for name in fixture_names:
            for row in rows:
                if row["fixture"] != name:
                    continue
                output.write(
                    f"| `{name}` | `{row['backend']}` | {format_bytes(int(row['input_bytes']))} | "
                    f"{format_bytes(int(row['compressed_bytes']))} | {float(row['ratio']):.6f} | "
                    f"{float(row['compress_s']):.3f} | yes |\n"
                )


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compress, verify, decompress, and byte-compare real LDS fixtures."
    )
    parser.add_argument("--binary", default="build/ld-compress-ng", type=Path)
    parser.add_argument("--fixtures", default=DEFAULT_FIXTURE_DIR, type=Path)
    parser.add_argument("--out-dir", default="build/real-fixture-roundtrips", type=Path)
    parser.add_argument("--backends", default=",".join(DEFAULT_BACKENDS), type=parse_backends,
        help="comma-separated backends to test: cpu,native-fixed,opencl,vulkan,metal")
    parser.add_argument("--frame-samples", default="4608",
        type=uint_arg("frame samples", 16, 4608))
    parser.add_argument("--lpc-order", default="12", type=uint_arg("LPC order", 0, 12))
    parser.add_argument("--lpc-precision", default="12", type=uint_arg("LPC precision", 1, 15))
    parser.add_argument("--rice-partition-order", default="5",
        type=uint_arg("Rice partition order", 0, 8))
    parser.add_argument("--native-threads", default="8", type=uint_arg("native threads", 1, 1024))
    parser.add_argument("--opencl-device", help="OpenCL backend-local device index")
    parser.add_argument("--vulkan-device", help="Vulkan backend-local device index")
    parser.add_argument("--metal-device", help="Metal backend-local device index")
    parser.add_argument("--limit", type=positive_int,
        help="round-trip only the first N fixtures after path sorting")
    parser.add_argument("--dry-run", action="store_true",
        help="print commands without running them")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    binary = args.binary
    fixture_root = args.fixtures
    backends = args.backends

    if not binary.is_file():
        raise RuntimeError(f"binary not found: {binary}")
    if not fixture_root.is_dir():
        raise RuntimeError(f"fixture directory not found: {fixture_root}")
    if args.opencl_device is not None and "opencl" not in backends:
        raise RuntimeError("--opencl-device requires the opencl backend")
    if args.vulkan_device is not None and "vulkan" not in backends:
        raise RuntimeError("--vulkan-device requires the vulkan backend")
    if args.metal_device is not None and "metal" not in backends:
        raise RuntimeError("--metal-device requires the metal backend")

    fixtures = find_fixtures(fixture_root, args.limit)
    if not fixtures:
        raise RuntimeError(f"no .lds fixtures found under {fixture_root}")

    config = RunConfig(
        frame_samples=args.frame_samples,
        lpc_order=args.lpc_order,
        lpc_precision=args.lpc_precision,
        rice_partition_order=args.rice_partition_order,
        native_threads=args.native_threads,
        opencl_device=args.opencl_device,
        vulkan_device=args.vulkan_device,
        metal_device=args.metal_device,
    )

    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    run_dir = args.out_dir / f"real-fixture-roundtrip-{stamp}"
    csv_path = run_dir / f"real-fixture-roundtrip-{stamp}.csv"
    markdown_path = run_dir / f"real-fixture-roundtrip-{stamp}.md"

    if args.dry_run:
        for fixture in fixtures:
            for backend in backends:
                compressed, decoded = output_paths(run_dir, fixture_root, fixture, backend)
                print(" ".join(compress_command(binary, backend, fixture, compressed, config)))
                print(" ".join(verify_command(binary, fixture, compressed)))
                print(" ".join(decompress_command(binary, compressed, decoded)))
        return 0

    run_dir.mkdir(parents=True, exist_ok=True)
    rows: list[dict[str, str]] = []
    total = len(fixtures) * len(backends)
    completed = 0
    for fixture in fixtures:
        rel = fixture.relative_to(fixture_root).as_posix()
        for backend in backends:
            completed += 1
            print(f"[{completed}/{total}] {backend} {rel}", flush=True)
            row = roundtrip_fixture(binary, fixture_root, run_dir, fixture, backend, config)
            rows.append(row)
            print(
                f"    {row['compressed_bytes']} bytes, ratio {row['ratio']}, "
                f"compress {row['compress_s']}s, verify {row['verify_s']}s, "
                f"decompress {row['decompress_s']}s",
                flush=True,
            )

    write_csv(csv_path, rows)
    write_markdown(markdown_path, rows, fixtures, fixture_root)
    print(f"wrote {csv_path}")
    print(f"wrote {markdown_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except (RuntimeError, OSError, subprocess.SubprocessError) as exc:
        print(f"roundtrip_real_fixtures.py: {exc}", file=sys.stderr)
        raise SystemExit(1)
