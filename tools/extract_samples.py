#!/usr/bin/env python3
"""Extract sampling profiler data from QEMU serial dump and generate HTML report.

Usage:
    python3 tools/extract_samples.py build/profiling/<class>/samples.raw build/profiling/<class>

Expects the kernel to emit frames: [4 bytes "SMPL"] [4 byte LE count] [8 byte IPs...]
The script correlates addresses with ELF symbols and generates an HTML coverage report.
"""
import struct
import sys
import os
import subprocess
import html


def main() -> None:
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <samples.raw> <output_dir>")
        sys.exit(1)

    raw_path = sys.argv[1]
    output_dir = sys.argv[2]
    if not os.path.exists(raw_path):
        print(f"Error: {raw_path} not found")
        sys.exit(1)

    data = open(raw_path, "rb").read()
    build_dir = os.path.realpath(os.path.join(os.path.dirname(raw_path), "..", ".."))

    # Find SMPL marker
    idx = data.find(b"SMPL")
    if idx < 0:
        print("Error: no SMPL marker found in serial dump")
        sys.exit(1)

    idx += 4
    count = struct.unpack("<I", data[idx : idx + 4])[0]
    idx += 4

    samples = []
    for i in range(count):
        if idx + 8 > len(data):
            break
        ip = struct.unpack("<Q", data[idx : idx + 8])[0]
        samples.append(ip)
        idx += 8

    print(f"Extracted {len(samples)} sample(s) from serial dump")

    # Get symbol table from kernel ELF
    kernel_elf = os.path.join(build_dir, "kernel.elf")
    if not os.path.exists(kernel_elf):
        print(f"Error: {kernel_elf} not found")
        sys.exit(1)

    result = subprocess.run(
        ["x86_64-elf-nm", "-n", kernel_elf],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print(f"nm error: {result.stderr}")
        sys.exit(1)

    # Build address -> symbol mapping
    addr_to_sym = {}
    func_ranges = []  # (start, end, name)
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            try:
                addr = int(parts[0], 16)
                sym_type = parts[1]
                name = " ".join(parts[2:])
                if sym_type in ("T", "t", "W", "w"):
                    addr_to_sym[addr] = name
                    func_ranges.append((addr, 0, name))  # end will be filled below
            except ValueError:
                pass

    # Sort by address and compute function ends
    func_ranges.sort(key=lambda x: x[0])
    for i in range(len(func_ranges) - 1):
        func_ranges[i] = (func_ranges[i][0], func_ranges[i+1][0], func_ranges[i][2])
    if func_ranges:
        func_ranges[-1] = (func_ranges[-1][0], func_ranges[-1][0] + 0x1000, func_ranges[-1][2])

    # Map samples to functions
    func_counts = {}
    for ip in samples:
        found = False
        for start, end, name in func_ranges:
            if ip >= start and (end == 0 or ip < end):
                func_counts[name] = func_counts.get(name, 0) + 1
                found = True
                break
        if not found:
            func_counts["[unknown]"] = func_counts.get("[unknown]", 0) + 1

    total = len(samples)
    
    # Sort functions by count descending
    sorted_funcs = sorted(func_counts.items(), key=lambda x: x[1], reverse=True)

    # Generate HTML report
    os.makedirs(output_dir, exist_ok=True)

    def fmt_addr(a):
        return f"0x{a:016x}"

    rows = ""
    for name, count in sorted_funcs:
        pct = (count / total * 100) if total > 0 else 0
        color = "#4CAF50" if count > 0 else "#f44336"
        rows += f"""<tr style="background-color: {color}22">
            <td><code>{html.escape(name)}</code></td>
            <td>{count}</td>
            <td>{pct:.1f}%</td>
        </tr>\n"""

    html_content = f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>Jarvis RTOS Sampling Profile Report</title>
<style>
body {{ font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; margin: 20px; }}
h1 {{ color: #333; }}
.summary {{ margin: 20px 0; padding: 15px; background: #f5f5f5; border-radius: 8px; }}
.summary span {{ font-weight: bold; }}
.green {{ color: #4CAF50; }}
.red {{ color: #f44336; }}
table {{ border-collapse: collapse; width: 100%; }}
th, td {{ text-align: left; padding: 8px; border-bottom: 1px solid #ddd; }}
th {{ background-color: #333; color: white; }}
tr:hover {{ background-color: #f5f5f5 !important; }}
</style>
</head>
<body>
<h1>Jarvis RTOS Sampling Profile Report</h1>
<div class="summary">
    <p>Total samples: <span>{total}</span></p>
    <p>Functions hit: <span class="green">{len(func_counts)}</span></p>
</div>
<table>
<tr><th>Function</th><th>Samples</th><th>%</th></tr>
{rows}
</table>
</body>
</html>"""

    report_path = os.path.join(output_dir, "index.html")
    with open(report_path, "w") as f:
        f.write(html_content)

    print(f"Profile report: {report_path}")
    print(f"  {len(samples)} samples, {len(func_counts)} functions hit")


if __name__ == "__main__":
    main()