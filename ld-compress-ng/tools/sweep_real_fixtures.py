#!/usr/bin/env python3
"""Run ld-compress-ng benchmark sweeps across ignored real LDS fixtures."""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import subprocess
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


DEFAULT_FIXTURE_DIR = (
    "reference/testdata/ld-decode-testdata-ci/"
    "1cf698d2025e8515e9ef57e34adaf85a194da96a"
)
TIMING_COLUMNS = [
    "accel_total_s",
    "accel_setup_s",
    "accel_scan_s",
    "accel_scan_read_s",
    "accel_scan_decode_s",
    "accel_scan_md5_s",
    "accel_analyze_s",
    "accel_plan_s",
    "accel_exact_s",
    "ocl_plan_guess_s",
    "ocl_plan_fill_s",
    "ocl_setup_dev_s",
    "ocl_setup_ctx_s",
    "ocl_setup_q_s",
    "ocl_setup_src_s",
    "ocl_setup_build_s",
    "ocl_setup_kernel_s",
    "writer_total_s",
    "tail_write_s",
    "writer_val_s",
    "writer_shift_s",
    "writer_resid_s",
    "writer_rice_s",
    "writer_bits_s",
    "writer_out_s",
    "opencl_up_s",
    "opencl_waste_s",
    "opencl_ac_s",
    "opencl_lpc_s",
    "opencl_quant_s",
    "opencl_fguess_s",
    "opencl_exact_s",
    "opencl_choose_s",
    "opencl_read_s",
    "vk_gpu_total_s",
    "vk_gpu_up_s",
    "vk_gpu_prep_s",
    "vk_gpu_ac_s",
    "vk_gpu_lpc_s",
    "vk_gpu_quant_s",
    "vk_gpu_fguess_s",
    "vk_gpu_exact_s",
    "vk_gpu_choose_s",
    "vk_gpu_read_s",
    "metal_up_s",
    "metal_gen_s",
    "metal_waste_s",
    "metal_ac_s",
    "metal_lpc_s",
    "metal_quant_s",
    "metal_fguess_s",
    "metal_exact_s",
    "metal_choose_s",
    "metal_read_s",
]
CSV_COLUMNS = [
    "fixture",
    "backend",
    "threads",
    "frame_samples",
    "lpc_order",
    "lpc_precision",
    "rice_order",
    "profile",
    "input_bytes",
    "output_bytes",
    "samples",
    "ratio",
    "elapsed_s",
    "mib_per_s",
    "subframes",
    "lpc_orders",
    "rice_orders",
    "wasted_bits",
    *TIMING_COLUMNS,
]
BENCH_COLUMNS = [
    "backend",
    "threads",
    "frame_samples",
    "lpc_order",
    "lpc_prec",
    "rice_order",
    "profile",
    "input_bytes",
    "output_bytes",
    "samples",
    "ratio",
    "elapsed_s",
    "mib_per_s",
    "subframes",
    "lpc_orders",
    "rice_orders",
    "wasted_bits",
    *TIMING_COLUMNS,
]


@dataclass(frozen=True)
class SweepConfig:
    threads: str
    frame_samples: str
    lpc_order: str
    lpc_precision: str
    rice_partition_order: str
    analysis_profile: str
    include_opencl: bool
    opencl_device: str | None
    reuse_opencl_session: bool
    include_vulkan: bool
    vulkan_device: str | None
    reuse_vulkan_session: bool
    include_metal: bool
    metal_device: str | None
    reuse_metal_session: bool


def parse_uint_list(text: str, name: str, minimum: int, maximum: int) -> list[int]:
    values: list[int] = []
    if not text:
        raise argparse.ArgumentTypeError(f"{name} cannot be empty")
    for item in text.split(","):
        if not item:
            raise argparse.ArgumentTypeError(f"{name} contains an empty item")
        try:
            value = int(item, 10)
        except ValueError as exc:
            raise argparse.ArgumentTypeError(f"{name} contains a non-integer item: {item}") from exc
        if value < minimum or value > maximum:
            raise argparse.ArgumentTypeError(f"{name} values must be {minimum}..{maximum}: {value}")
        values.append(value)
    return values


def uint_list_arg(name: str, minimum: int, maximum: int):
    def parse(text: str) -> str:
        parse_uint_list(text, name, minimum, maximum)
        return text

    return parse


VALID_ANALYSIS_PROFILES = {
    "exact",
    "order-guess-exact-rice",
    "order-guess-mean-rice",
    "order-guess-mean-estimate-rice",
    "subdivide-tukey3-mean-rice",
    "subdivide-tukey3-mean-estimate-rice",
}


def analysis_profile_arg(text: str) -> str:
    if not text:
        raise argparse.ArgumentTypeError("analysis profile list cannot be empty")
    for item in text.split(","):
        if item not in VALID_ANALYSIS_PROFILES:
            raise argparse.ArgumentTypeError(f"unknown analysis profile: {item}")
    return text


def find_fixtures(root: Path, limit: int | None) -> list[Path]:
    fixtures = sorted(path for path in root.rglob("*.lds") if path.is_file())
    if limit is not None:
        fixtures = fixtures[:limit]
    return fixtures


def bench_command(binary: Path, config: SweepConfig, fixture: Path) -> list[str]:
    command = [
        str(binary),
        "bench",
        "--threads",
        config.threads,
        "--frame-samples",
        config.frame_samples,
        "--lpc-order",
        config.lpc_order,
        "--lpc-precision",
        config.lpc_precision,
        "--rice-partition-order",
        config.rice_partition_order,
        "--analysis-profile",
        config.analysis_profile,
    ]
    if config.include_opencl:
        command.append("--include-opencl")
        if config.reuse_opencl_session:
            command.append("--reuse-opencl-session")
        if config.opencl_device is not None:
            command.extend(["--opencl-device", config.opencl_device])
    if config.include_vulkan:
        command.append("--include-vulkan")
        if config.reuse_vulkan_session:
            command.append("--reuse-vulkan-session")
        if config.vulkan_device is not None:
            command.extend(["--vulkan-device", config.vulkan_device])
    if config.include_metal:
        command.append("--include-metal")
        if config.reuse_metal_session:
            command.append("--reuse-metal-session")
        if config.metal_device is not None:
            command.extend(["--metal-device", config.metal_device])
    command.append(str(fixture))
    return command


def parse_bench_output(output: str, fixture: str) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    lines = [line for line in output.splitlines() if line.strip()]
    if not lines:
        raise RuntimeError("bench produced no output")

    header = lines[0].split()
    if header != BENCH_COLUMNS:
        raise RuntimeError(
            "unexpected bench header: " + " ".join(header)
        )

    for line in lines[1:]:
        parts = line.split()
        if len(parts) != len(BENCH_COLUMNS):
            raise RuntimeError("could not parse bench row: " + line)
        row = dict(zip(BENCH_COLUMNS, parts))
        row["lpc_precision"] = row.pop("lpc_prec")
        row["fixture"] = fixture
        rows.append({column: row[column] for column in CSV_COLUMNS})
    return rows


def run_bench(binary: Path, config: SweepConfig, fixture: Path, root: Path) -> list[dict[str, str]]:
    command = bench_command(binary, config, fixture)
    completed = subprocess.run(
        command,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if completed.returncode != 0:
        if completed.stdout:
            print(completed.stdout, end="", file=sys.stderr)
        if completed.stderr:
            print(completed.stderr, end="", file=sys.stderr)
        raise RuntimeError(f"bench failed for {fixture} with exit code {completed.returncode}")
    return parse_bench_output(completed.stdout, fixture.relative_to(root).as_posix())


def numeric(row: dict[str, str], column: str) -> float:
    return float(row[column])


def int_field(row: dict[str, str], column: str) -> int:
    return int(row[column])


def native_key(row: dict[str, str]) -> tuple[str, str, str, str, str, str]:
    return (
        row["threads"],
        row["frame_samples"],
        row["lpc_order"],
        row["lpc_precision"],
        row["rice_order"],
        row["profile"],
    )


def format_bytes(value: int) -> str:
    return f"{value:,}"


def sorted_native_rows(rows: Iterable[dict[str, str]]) -> list[dict[str, str]]:
    return sorted(
        (row for row in rows if row["backend"] == "native-fixed"),
        key=lambda row: (int_field(row, "output_bytes"), numeric(row, "elapsed_s")),
    )


def sorted_opencl_rows(rows: Iterable[dict[str, str]]) -> list[dict[str, str]]:
    return sorted(
        (row for row in rows if row["backend"] == "opencl"),
        key=lambda row: (int_field(row, "output_bytes"), numeric(row, "elapsed_s")),
    )


def sorted_vulkan_rows(rows: Iterable[dict[str, str]]) -> list[dict[str, str]]:
    return sorted(
        (row for row in rows if row["backend"] == "vulkan"),
        key=lambda row: (int_field(row, "output_bytes"), numeric(row, "elapsed_s")),
    )


def sorted_metal_rows(rows: Iterable[dict[str, str]]) -> list[dict[str, str]]:
    return sorted(
        (row for row in rows if row["backend"] == "metal"),
        key=lambda row: (int_field(row, "output_bytes"), numeric(row, "elapsed_s")),
    )


def write_csv(path: Path, rows: list[dict[str, str]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=CSV_COLUMNS)
        writer.writeheader()
        writer.writerows(rows)


def write_markdown(
    path: Path,
    rows: list[dict[str, str]],
    fixtures: list[Path],
    root: Path,
    config: SweepConfig,
) -> None:
    by_fixture: dict[str, list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        by_fixture[row["fixture"]].append(row)

    fixture_names = [fixture.relative_to(root).as_posix() for fixture in fixtures]
    cpu_by_fixture = {
        name: next(row for row in by_fixture[name] if row["backend"] == "cpu")
        for name in fixture_names
    }

    native_groups: dict[tuple[str, str, str, str, str, str], list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        if row["backend"] == "native-fixed":
            native_groups[native_key(row)].append(row)

    complete_groups = [
        (key, group)
        for key, group in native_groups.items()
        if len(group) == len(fixture_names)
    ]
    complete_groups.sort(key=lambda item: (
        sum(int_field(row, "output_bytes") for row in item[1]),
        sum(numeric(row, "elapsed_s") for row in item[1]),
    ))

    opencl_groups: dict[tuple[str, str, str, str, str, str], list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        if row["backend"] == "opencl":
            opencl_groups[native_key(row)].append(row)
    complete_opencl_groups = [
        (key, group)
        for key, group in opencl_groups.items()
        if len(group) == len(fixture_names)
    ]
    complete_opencl_groups.sort(key=lambda item: (
        sum(int_field(row, "output_bytes") for row in item[1]),
        sum(numeric(row, "elapsed_s") for row in item[1]),
    ))

    vulkan_groups: dict[tuple[str, str, str, str, str, str], list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        if row["backend"] == "vulkan":
            vulkan_groups[native_key(row)].append(row)
    complete_vulkan_groups = [
        (key, group)
        for key, group in vulkan_groups.items()
        if len(group) == len(fixture_names)
    ]
    complete_vulkan_groups.sort(key=lambda item: (
        sum(int_field(row, "output_bytes") for row in item[1]),
        sum(numeric(row, "elapsed_s") for row in item[1]),
    ))

    metal_groups: dict[tuple[str, str, str, str, str, str], list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        if row["backend"] == "metal":
            metal_groups[native_key(row)].append(row)
    complete_metal_groups = [
        (key, group)
        for key, group in metal_groups.items()
        if len(group) == len(fixture_names)
    ]
    complete_metal_groups.sort(key=lambda item: (
        sum(int_field(row, "output_bytes") for row in item[1]),
        sum(numeric(row, "elapsed_s") for row in item[1]),
    ))

    total_cpu_bytes = sum(int_field(row, "output_bytes") for row in cpu_by_fixture.values())

    with path.open("w", encoding="utf-8") as output:
        output.write("# Real Fixture Native Tuning Sweep\n\n")
        output.write(f"- Fixtures: `{root}`\n")
        output.write(f"- Fixture count: {len(fixture_names)}\n")
        output.write(f"- Threads: `{config.threads}`\n")
        output.write(f"- Frame samples: `{config.frame_samples}`\n")
        output.write(f"- LPC orders: `{config.lpc_order}`\n")
        output.write(f"- LPC precisions: `{config.lpc_precision}`\n")
        output.write(f"- Rice partition orders: `{config.rice_partition_order}`\n")
        output.write(f"- Analysis profiles: `{config.analysis_profile}`\n")
        output.write(f"- OpenCL included: `{str(config.include_opencl).lower()}`\n")
        output.write(
            f"- OpenCL session reuse: `{str(config.reuse_opencl_session).lower()}`\n"
        )
        if config.opencl_device is not None:
            output.write(f"- OpenCL device: `{config.opencl_device}`\n")
        output.write(f"- Vulkan included: `{str(config.include_vulkan).lower()}`\n")
        output.write(
            f"- Vulkan session reuse: `{str(config.reuse_vulkan_session).lower()}`\n"
        )
        if config.vulkan_device is not None:
            output.write(f"- Vulkan device: `{config.vulkan_device}`\n")
        output.write(f"- Metal included: `{str(config.include_metal).lower()}`\n")
        output.write(
            f"- Metal session reuse: `{str(config.reuse_metal_session).lower()}`\n"
        )
        if config.metal_device is not None:
            output.write(f"- Metal device: `{config.metal_device}`\n")
        output.write("\n")

        output.write("## Best Native Per Fixture\n\n")
        output.write("| Fixture | CPU bytes | Native bytes | Native ratio | Gap vs CPU | Settings |\n")
        output.write("| --- | ---: | ---: | ---: | ---: | --- |\n")
        for name in fixture_names:
            cpu = cpu_by_fixture[name]
            best = sorted_native_rows(by_fixture[name])[0]
            cpu_bytes = int_field(cpu, "output_bytes")
            native_bytes = int_field(best, "output_bytes")
            gap = ((native_bytes / cpu_bytes) - 1.0) * 100.0 if cpu_bytes else 0.0
            settings = (
                f"threads={best['threads']}, frame={best['frame_samples']}, "
                f"lpc={best['lpc_order']}, prec={best['lpc_precision']}, "
                f"rice={best['rice_order']}, profile={best['profile']}"
            )
            output.write(
                f"| `{name}` | {format_bytes(cpu_bytes)} | {format_bytes(native_bytes)} | "
                f"{float(best['ratio']):.4f} | {gap:+.2f}% | `{settings}` |\n"
            )

        if complete_opencl_groups:
            output.write("\n## Best OpenCL Per Fixture\n\n")
            output.write("| Fixture | CPU bytes | OpenCL bytes | OpenCL ratio | Gap vs CPU | Settings |\n")
            output.write("| --- | ---: | ---: | ---: | ---: | --- |\n")
            for name in fixture_names:
                cpu = cpu_by_fixture[name]
                best = sorted_opencl_rows(by_fixture[name])[0]
                cpu_bytes = int_field(cpu, "output_bytes")
                opencl_bytes = int_field(best, "output_bytes")
                gap = ((opencl_bytes / cpu_bytes) - 1.0) * 100.0 if cpu_bytes else 0.0
                settings = (
                    f"threads={best['threads']}, frame={best['frame_samples']}, "
                    f"lpc={best['lpc_order']}, prec={best['lpc_precision']}, "
                    f"rice={best['rice_order']}"
                )
                output.write(
                    f"| `{name}` | {format_bytes(cpu_bytes)} | {format_bytes(opencl_bytes)} | "
                    f"{float(best['ratio']):.4f} | {gap:+.2f}% | `{settings}` |\n"
                )

        if complete_vulkan_groups:
            output.write("\n## Best Vulkan Per Fixture\n\n")
            output.write("| Fixture | CPU bytes | Vulkan bytes | Vulkan ratio | Gap vs CPU | Settings |\n")
            output.write("| --- | ---: | ---: | ---: | ---: | --- |\n")
            for name in fixture_names:
                cpu = cpu_by_fixture[name]
                best = sorted_vulkan_rows(by_fixture[name])[0]
                cpu_bytes = int_field(cpu, "output_bytes")
                vulkan_bytes = int_field(best, "output_bytes")
                gap = ((vulkan_bytes / cpu_bytes) - 1.0) * 100.0 if cpu_bytes else 0.0
                settings = (
                    f"threads={best['threads']}, frame={best['frame_samples']}, "
                    f"lpc={best['lpc_order']}, prec={best['lpc_precision']}, "
                    f"rice={best['rice_order']}"
                )
                output.write(
                    f"| `{name}` | {format_bytes(cpu_bytes)} | {format_bytes(vulkan_bytes)} | "
                    f"{float(best['ratio']):.4f} | {gap:+.2f}% | `{settings}` |\n"
                )

        if complete_metal_groups:
            output.write("\n## Best Metal Per Fixture\n\n")
            output.write("| Fixture | CPU bytes | Metal bytes | Metal ratio | Gap vs CPU | Settings |\n")
            output.write("| --- | ---: | ---: | ---: | ---: | --- |\n")
            for name in fixture_names:
                cpu = cpu_by_fixture[name]
                best = sorted_metal_rows(by_fixture[name])[0]
                cpu_bytes = int_field(cpu, "output_bytes")
                metal_bytes = int_field(best, "output_bytes")
                gap = ((metal_bytes / cpu_bytes) - 1.0) * 100.0 if cpu_bytes else 0.0
                settings = (
                    f"threads={best['threads']}, frame={best['frame_samples']}, "
                    f"lpc={best['lpc_order']}, prec={best['lpc_precision']}, "
                    f"rice={best['rice_order']}"
                )
                output.write(
                    f"| `{name}` | {format_bytes(cpu_bytes)} | {format_bytes(metal_bytes)} | "
                    f"{float(best['ratio']):.4f} | {gap:+.2f}% | `{settings}` |\n"
                )

        output.write("\n## Aggregate Native Configs\n\n")
        output.write("| Rank | Native bytes | Gap vs CPU | Elapsed s | Settings |\n")
        output.write("| ---: | ---: | ---: | ---: | --- |\n")
        for rank, (key, group) in enumerate(complete_groups[:10], start=1):
            native_bytes = sum(int_field(row, "output_bytes") for row in group)
            elapsed = sum(numeric(row, "elapsed_s") for row in group)
            gap = ((native_bytes / total_cpu_bytes) - 1.0) * 100.0 if total_cpu_bytes else 0.0
            threads, frame_samples, lpc_order, lpc_precision, rice_order, profile = key
            settings = (
                f"threads={threads}, frame={frame_samples}, lpc={lpc_order}, "
                f"prec={lpc_precision}, rice={rice_order}, profile={profile}"
            )
            output.write(
                f"| {rank} | {format_bytes(native_bytes)} | {gap:+.2f}% | "
                f"{elapsed:.3f} | `{settings}` |\n"
            )

        if complete_opencl_groups:
            output.write("\n## Aggregate OpenCL Configs\n\n")
            output.write("| Rank | OpenCL bytes | Gap vs CPU | Elapsed s | Settings |\n")
            output.write("| ---: | ---: | ---: | ---: | --- |\n")
            for rank, (key, group) in enumerate(complete_opencl_groups[:10], start=1):
                opencl_bytes = sum(int_field(row, "output_bytes") for row in group)
                elapsed = sum(numeric(row, "elapsed_s") for row in group)
                gap = ((opencl_bytes / total_cpu_bytes) - 1.0) * 100.0 if total_cpu_bytes else 0.0
                threads, frame_samples, lpc_order, lpc_precision, rice_order, profile = key
                settings = (
                    f"threads={threads}, frame={frame_samples}, lpc={lpc_order}, "
                    f"prec={lpc_precision}, rice={rice_order}, profile={profile}"
                )
                output.write(
                    f"| {rank} | {format_bytes(opencl_bytes)} | {gap:+.2f}% | "
                    f"{elapsed:.3f} | `{settings}` |\n"
                )

        if complete_vulkan_groups:
            output.write("\n## Aggregate Vulkan Configs\n\n")
            output.write("| Rank | Vulkan bytes | Gap vs CPU | Elapsed s | Settings |\n")
            output.write("| ---: | ---: | ---: | ---: | --- |\n")
            for rank, (key, group) in enumerate(complete_vulkan_groups[:10], start=1):
                vulkan_bytes = sum(int_field(row, "output_bytes") for row in group)
                elapsed = sum(numeric(row, "elapsed_s") for row in group)
                gap = ((vulkan_bytes / total_cpu_bytes) - 1.0) * 100.0 if total_cpu_bytes else 0.0
                threads, frame_samples, lpc_order, lpc_precision, rice_order, profile = key
                settings = (
                    f"threads={threads}, frame={frame_samples}, lpc={lpc_order}, "
                    f"prec={lpc_precision}, rice={rice_order}, profile={profile}"
                )
                output.write(
                    f"| {rank} | {format_bytes(vulkan_bytes)} | {gap:+.2f}% | "
                    f"{elapsed:.3f} | `{settings}` |\n"
                )

        if complete_metal_groups:
            output.write("\n## Aggregate Metal Configs\n\n")
            output.write("| Rank | Metal bytes | Gap vs CPU | Elapsed s | Settings |\n")
            output.write("| ---: | ---: | ---: | ---: | --- |\n")
            for rank, (key, group) in enumerate(complete_metal_groups[:10], start=1):
                metal_bytes = sum(int_field(row, "output_bytes") for row in group)
                elapsed = sum(numeric(row, "elapsed_s") for row in group)
                gap = ((metal_bytes / total_cpu_bytes) - 1.0) * 100.0 if total_cpu_bytes else 0.0
                threads, frame_samples, lpc_order, lpc_precision, rice_order, profile = key
                settings = (
                    f"threads={threads}, frame={frame_samples}, lpc={lpc_order}, "
                    f"prec={lpc_precision}, rice={rice_order}, profile={profile}"
                )
                output.write(
                    f"| {rank} | {format_bytes(metal_bytes)} | {gap:+.2f}% | "
                    f"{elapsed:.3f} | `{settings}` |\n"
                )


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run ld-compress-ng bench across a tree of real LDS fixtures."
    )
    parser.add_argument("--binary", default="build/ld-compress-ng", type=Path)
    parser.add_argument("--fixtures", default=DEFAULT_FIXTURE_DIR, type=Path)
    parser.add_argument("--out-dir", default="build/real-fixture-sweeps", type=Path)
    parser.add_argument("--threads", default="8", type=uint_list_arg("threads", 1, 1024))
    parser.add_argument("--frame-samples", default="4608",
        type=uint_list_arg("frame samples", 16, 4608))
    parser.add_argument("--lpc-order", default="10,12",
        type=uint_list_arg("LPC order", 0, 12))
    parser.add_argument("--lpc-precision", default="10,12",
        type=uint_list_arg("LPC precision", 1, 15))
    parser.add_argument("--rice-partition-order", default="5",
        type=uint_list_arg("Rice partition order", 0, 8))
    parser.add_argument("--analysis-profile", default="exact",
        type=analysis_profile_arg)
    parser.add_argument("--include-opencl", action="store_true",
        help="include OpenCL backend rows in each bench run")
    parser.add_argument("--reuse-opencl-session", action="store_true",
        help="reuse one OpenCL analysis session across OpenCL bench rows")
    parser.add_argument("--opencl-device",
        help="flattened OpenCL device index to pass to bench when --include-opencl is set")
    parser.add_argument("--include-vulkan", action="store_true",
        help="include Vulkan backend rows in each bench run")
    parser.add_argument("--reuse-vulkan-session", action="store_true",
        help="reuse one Vulkan analysis session across Vulkan bench rows")
    parser.add_argument("--vulkan-device",
        help="Vulkan device index to pass to bench when --include-vulkan is set")
    parser.add_argument("--include-metal", action="store_true",
        help="include Metal backend rows in each bench run")
    parser.add_argument("--reuse-metal-session", action="store_true",
        help="reuse one Metal analysis session across Metal bench rows")
    parser.add_argument("--metal-device",
        help="Metal device index to pass to bench when --include-metal is set")
    parser.add_argument("--limit", type=int,
        help="benchmark only the first N fixtures after path sorting")
    parser.add_argument("--dry-run", action="store_true",
        help="print bench commands without running them")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    binary = args.binary
    fixture_root = args.fixtures

    if args.limit is not None and args.limit <= 0:
        raise RuntimeError("--limit must be positive")
    if not binary.is_file():
        raise RuntimeError(f"binary not found: {binary}")
    if not fixture_root.is_dir():
        raise RuntimeError(f"fixture directory not found: {fixture_root}")

    fixtures = find_fixtures(fixture_root, args.limit)
    if not fixtures:
        raise RuntimeError(f"no .lds fixtures found under {fixture_root}")

    config = SweepConfig(
        threads=args.threads,
        frame_samples=args.frame_samples,
        lpc_order=args.lpc_order,
        lpc_precision=args.lpc_precision,
        rice_partition_order=args.rice_partition_order,
        analysis_profile=args.analysis_profile,
        include_opencl=args.include_opencl,
        opencl_device=args.opencl_device,
        reuse_opencl_session=args.reuse_opencl_session,
        include_vulkan=args.include_vulkan,
        vulkan_device=args.vulkan_device,
        reuse_vulkan_session=args.reuse_vulkan_session,
        include_metal=args.include_metal,
        metal_device=args.metal_device,
        reuse_metal_session=args.reuse_metal_session,
    )
    if args.opencl_device is not None and not args.include_opencl:
        raise RuntimeError("--opencl-device requires --include-opencl")
    if args.reuse_opencl_session and not args.include_opencl:
        raise RuntimeError("--reuse-opencl-session requires --include-opencl")
    if args.vulkan_device is not None and not args.include_vulkan:
        raise RuntimeError("--vulkan-device requires --include-vulkan")
    if args.reuse_vulkan_session and not args.include_vulkan:
        raise RuntimeError("--reuse-vulkan-session requires --include-vulkan")
    if args.metal_device is not None and not args.include_metal:
        raise RuntimeError("--metal-device requires --include-metal")
    if args.reuse_metal_session and not args.include_metal:
        raise RuntimeError("--reuse-metal-session requires --include-metal")

    if args.dry_run:
        for fixture in fixtures:
            print(" ".join(bench_command(binary, config, fixture)))
        return 0

    args.out_dir.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    csv_path = args.out_dir / f"real-fixture-sweep-{stamp}.csv"
    markdown_path = args.out_dir / f"real-fixture-sweep-{stamp}.md"

    rows: list[dict[str, str]] = []
    for index, fixture in enumerate(fixtures, start=1):
        rel = fixture.relative_to(fixture_root).as_posix()
        print(f"[{index}/{len(fixtures)}] {rel}", flush=True)
        fixture_rows = run_bench(binary, config, fixture, fixture_root)
        rows.extend(fixture_rows)
        best = sorted_native_rows(fixture_rows)[0]
        print(
            "    best native-fixed: "
            f"{best['output_bytes']} bytes, ratio {best['ratio']}, "
            f"frame={best['frame_samples']} lpc={best['lpc_order']} "
            f"prec={best['lpc_precision']} rice={best['rice_order']} "
            f"profile={best['profile']} threads={best['threads']}",
            flush=True,
        )
        opencl_rows = sorted_opencl_rows(fixture_rows)
        if opencl_rows:
            best_opencl = opencl_rows[0]
            print(
                "    best opencl: "
                f"{best_opencl['output_bytes']} bytes, ratio {best_opencl['ratio']}, "
                f"frame={best_opencl['frame_samples']} lpc={best_opencl['lpc_order']} "
                f"prec={best_opencl['lpc_precision']} rice={best_opencl['rice_order']}",
                flush=True,
            )
        vulkan_rows = sorted_vulkan_rows(fixture_rows)
        if vulkan_rows:
            best_vulkan = vulkan_rows[0]
            print(
                "    best vulkan: "
                f"{best_vulkan['output_bytes']} bytes, ratio {best_vulkan['ratio']}, "
                f"frame={best_vulkan['frame_samples']} lpc={best_vulkan['lpc_order']} "
                f"prec={best_vulkan['lpc_precision']} rice={best_vulkan['rice_order']}",
                flush=True,
            )
        metal_rows = sorted_metal_rows(fixture_rows)
        if metal_rows:
            best_metal = metal_rows[0]
            print(
                "    best metal: "
                f"{best_metal['output_bytes']} bytes, ratio {best_metal['ratio']}, "
                f"frame={best_metal['frame_samples']} lpc={best_metal['lpc_order']} "
                f"prec={best_metal['lpc_precision']} rice={best_metal['rice_order']}",
                flush=True,
            )

    write_csv(csv_path, rows)
    write_markdown(markdown_path, rows, fixtures, fixture_root, config)
    print(f"wrote {csv_path}")
    print(f"wrote {markdown_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except (RuntimeError, OSError, subprocess.SubprocessError) as exc:
        print(f"sweep_real_fixtures.py: {exc}", file=sys.stderr)
        raise SystemExit(1)
