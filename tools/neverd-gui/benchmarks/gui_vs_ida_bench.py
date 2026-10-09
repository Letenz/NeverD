#!/usr/bin/env python3
"""Time the workbench's window against IDA's, from launch, on one display.

Both programs start fresh on a fresh copy of each input, on a private X
server, with the default preferences: the workbench with temporary settings
and a fresh layout, IDA with a scratch user directory that holds a copy of
the license and configuration (no plug-ins, no history).

Milestones, from the launch the runner timed:
- the workbench: its first painted frame, and the first painted listing of
  the opened file (the startup report the GUI writes with
  --startup-benchmark, whose clock origin sits on the monotonic clock);
- IDA: the start of a script it runs once the file is open in its window
  (-S), and the end of its auto-analysis, which that script waits for.

Every launch is timed whole as well.  OS file caches stay warm.
"""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import platform
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import time

IDA_SCRIPT = r'''
import json, time
started = time.monotonic()
import ida_auto, ida_pro, idautils, idc
ida_auto.auto_wait()
analyzed = time.monotonic()
with open(idc.ARGV[1], "w") as report:
    json.dump(dict(script_started=started, analysis_done=analyzed,
                   functions=sum(1 for _ in idautils.Functions())), report)
ida_pro.qexit(0)
'''
# What IDA reads from the user directory; plug-ins stay out (one starts a server).
IDA_PROFILE = ("ida.reg", "idapro.hexlic", "ida-config.json", "cfg")


def percentiles(values):
    if not values:
        return {}
    ordered = sorted(values)

    def rank(p):
        return ordered[max(0, math.ceil(len(ordered) * p) - 1)]

    return dict(count=len(ordered), p50_ms=rank(.5), max_ms=ordered[-1], min_ms=ordered[0],
                mean_ms=statistics.mean(ordered))


def start_display(number):
    server = subprocess.Popen(["Xvfb", f":{number}", "-screen", "0", "1600x1000x24", "-nolisten",
                               "tcp"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        if Path(f"/tmp/.X11-unix/X{number}").exists():
            return server
        time.sleep(.05)
    server.kill()
    raise RuntimeError(f"Xvfb :{number} did not start")


def run_neverd(gui, worker, binary, display, scratch, timeout):
    report_path = scratch / "startup.json"
    report_path.unlink(missing_ok=True)
    environment = {key: value for key, value in os.environ.items()
                   if key not in ("WAYLAND_DISPLAY", "XAUTHORITY")}
    environment.update(DISPLAY=f":{display}", QT_QPA_PLATFORM="xcb", XDG_SESSION_TYPE="x11")
    command = [str(gui), "--worker", str(worker), "--startup-benchmark", str(report_path),
               "--startup-benchmark-timeout", str(round(timeout * 1000)), "--fresh-layout",
               str(binary)]
    launched = time.monotonic()
    subprocess.run(command, env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   timeout=timeout + 10)
    exited = time.monotonic()
    report = json.loads(report_path.read_text())
    if not report.get("success"):
        raise RuntimeError(f"the workbench did not finish: {report.get('failure_reason')}")
    offset = report["clock_origin_monotonic_ms"] - launched * 1e3
    milestones = report["milestones"]
    return dict(main_ms=offset, first_frame_ms=offset + milestones["first_frame_ms"],
                useful_frame_ms=offset + milestones["useful_frame_ms"],
                process_ms=(exited - launched) * 1e3, milestones=milestones)


def own_processes(home):
    """IDA processes started with this scratch HOME."""
    found = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            environment = (entry / "environ").read_bytes().split(b"\0")
        except OSError:
            continue
        if f"HOME={home}".encode() in environment:
            found.append(int(entry.name))
    return found


def run_ida(ida, binary, display, scratch, timeout):
    home = Path(tempfile.mkdtemp(prefix="ida-home-", dir=scratch))
    try:
        profile = home / ".idapro"
        profile.mkdir()
        for name in IDA_PROFILE:
            source = Path.home() / ".idapro" / name
            if source.is_dir():
                shutil.copytree(source, profile / name)
            elif source.exists():
                shutil.copy2(source, profile / name)
        copy = home / binary.name
        shutil.copyfile(binary, copy)
        script = home / "milestones.py"
        script.write_text(IDA_SCRIPT)
        output = home / "milestones.json"
        environment = {key: value for key, value in os.environ.items()
                       if key not in ("WAYLAND_DISPLAY", "XAUTHORITY")}
        environment.update(DISPLAY=f":{display}", QT_QPA_PLATFORM="xcb", GDK_BACKEND="x11",
                           XDG_SESSION_TYPE="x11", HOME=str(home), IDAUSR=str(profile),
                           XDG_RUNTIME_DIR=str(home))
        launched = time.monotonic()
        process = subprocess.Popen([str(ida), "-A", f"-S{script} {output}", str(copy)],
                                   env=environment, cwd=home, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL)
        try:
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            for pid in own_processes(home):
                os.kill(pid, signal.SIGKILL)
            raise RuntimeError(f"IDA did not finish {binary.name} in {timeout} s")
        exited = time.monotonic()
        milestones = json.loads(output.read_text())
        return dict(script_ms=(milestones["script_started"] - launched) * 1e3,
                    analysis_ms=(milestones["analysis_done"] - launched) * 1e3,
                    process_ms=(exited - launched) * 1e3, functions=milestones["functions"])
    finally:
        for pid in own_processes(home):
            try:
                os.kill(pid, signal.SIGKILL)
            except OSError:
                pass
        shutil.rmtree(home, ignore_errors=True)


def markdown(report):
    lines = [f"Launches per binary: {report['samples']} after {report['warmup']} warm-up; "
             f"display {report['display_size']}; load average at start "
             f"{report['load_average']}.", "",
             "| Binary | NeverD first frame | NeverD listing of the file | IDA window with the "
             "file (script start) | IDA auto-analysis done |", "|---|---:|---:|---:|---:|"]
    for row in report["binaries"]:
        n, i = row["neverd"], row["ida"]
        lines.append(f"| {row['name']} | {n['first_frame_ms'].get('p50_ms', 0):.0f} ms | "
                     f"{n['useful_frame_ms'].get('p50_ms', 0):.0f} ms | "
                     f"{i['script_ms'].get('p50_ms', 0):.0f} ms | "
                     f"{i['analysis_ms'].get('p50_ms', 0):.0f} ms |")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--gui", type=Path, required=True)
    parser.add_argument("--worker", type=Path, required=True)
    parser.add_argument("--ida", type=Path, required=True, help="IDA's GUI launcher")
    parser.add_argument("--display", type=int, default=95, help="private X display number")
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--markdown", type=Path)
    parser.add_argument("binaries", type=Path, nargs="+")
    args = parser.parse_args()
    report = dict(schema=1, kind="gui-startup-vs-ida", platform=platform.platform(),
                  cpus=os.cpu_count(), samples=args.samples, warmup=args.warmup,
                  display_size="1600x1000x24", load_average=[round(v, 1) for v in os.getloadavg()],
                  caveats=["Fresh processes, default preferences, warm OS caches.",
                           "IDA's script starts once the file is open in its window; its "
                           "first paint is not observed directly.",
                           "Paint milestones are not hardware presentation."],
                  binaries=[])
    server = start_display(args.display)
    try:
        with tempfile.TemporaryDirectory(prefix="gui-vs-ida-") as directory:
            scratch = Path(directory)
            for binary in args.binaries:
                binary = binary.resolve()
                samples = dict(neverd=[], ida=[])
                for index in range(args.warmup + args.samples):
                    print(f"{binary.name}: launch {index + 1}/{args.warmup + args.samples}",
                          file=sys.stderr)
                    copy = scratch / binary.name
                    shutil.copyfile(binary, copy)
                    neverd = run_neverd(args.gui.resolve(), args.worker.resolve(), copy,
                                        args.display, scratch, args.timeout)
                    ida = run_ida(args.ida.resolve(), binary, args.display, scratch, args.timeout)
                    if index >= args.warmup:
                        samples["neverd"].append(neverd)
                        samples["ida"].append(ida)
                row = dict(name=binary.name, size=binary.stat().st_size, samples=samples)
                row["neverd"] = {key: percentiles([s[key] for s in samples["neverd"]])
                                 for key in ("main_ms", "first_frame_ms", "useful_frame_ms",
                                             "process_ms")}
                row["ida"] = {key: percentiles([s[key] for s in samples["ida"]])
                              for key in ("script_ms", "analysis_ms", "process_ms")}
                report["binaries"].append(row)
    finally:
        server.terminate()
        server.wait(timeout=10)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if args.markdown:
        args.markdown.write_text(markdown(report), encoding="utf-8")
    print(markdown(report))
    return 0


if __name__ == "__main__":
    sys.exit(main())
