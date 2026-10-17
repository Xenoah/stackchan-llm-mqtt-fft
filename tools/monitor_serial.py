"""Record StackChan serial output without resetting it; reconnect after USB loss.

    python tools/monitor_serial.py --port COM4 --duration 1800

Logs and a JSON summary default to .pio/diagnostics (excluded from git).
Requires pyserial. A quiet or disconnected port is not proof of a firmware crash.
"""

from __future__ import annotations

import argparse
from datetime import datetime
import json
from pathlib import Path
import re
import time

import serial


FAULT = re.compile(
    r"Guru Meditation|panic(?:'ed)?|abort\(\)|Backtrace:|Stack canary|"
    r"stack overflow|assert failed|CORRUPT HEAP|watchdog|"
    r"Brownout detector|LoadProhibited|StoreProhibited|IllegalInstruction",
    re.IGNORECASE,
)
HEAP = re.compile(
    r"\[heap\] free=(\d+) min=(\d+) maxblk=(\d+) psram=(\d+)"
    r"(?: stack face=(\d+) loop=(\d+) mqtt=(\d+))?"
)
METRICS = ("free_heap", "min_free_heap", "max_block", "free_psram",
           "face_stack_free", "loop_stack_free", "mqtt_stack_free")


def stamp() -> str:
    return datetime.now().astimezone().isoformat(timespec="seconds")


class Summary:
    def __init__(self) -> None:
        self.data = {
            "started": stamp(), "port_opens": 0, "disconnects": 0,
            "open_failures": 0, "serial_lines": 0, "heap_samples": 0,
            "boot_lines": 0, "fault_lines": 0, "fault_examples": [], "metrics": {},
        }

    def line(self, text: str) -> bool:
        self.data["serial_lines"] += 1
        if "StackChan boot" in text:
            self.data["boot_lines"] += 1
        match = HEAP.search(text)
        if match:
            self.data["heap_samples"] += 1
            for key, raw in zip(METRICS, match.groups()):
                if raw is None:
                    continue
                value = int(raw)
                metric = self.data["metrics"].setdefault(
                    key, {"first": value, "min": value, "max": value, "last": value})
                metric["min"] = min(metric["min"], value)
                metric["max"] = max(metric["max"], value)
                metric["last"] = value
        fault = FAULT.search(text) is not None
        if fault:
            self.data["fault_lines"] += 1
            if len(self.data["fault_examples"]) < 20:
                self.data["fault_examples"].append(text)
        return fault


def record(port_name: str, duration: float, output: Path, baud: int = 115200) -> dict:
    output.parent.mkdir(parents=True, exist_ok=True)
    summary_path = output.with_suffix(".summary.json")
    if summary_path == output or summary_path.exists():
        raise FileExistsError(f"Choose a new log path: {summary_path}")
    stats = Summary()
    stats.data.update(port=port_name, duration_requested_s=duration)
    port = None
    pending = b""
    started = time.monotonic()
    deadline = started + duration
    next_attempt = started
    waiting_reported = False
    outcome = "completed"
    with output.open("x", encoding="utf-8", buffering=1) as log:
        def event(text: str) -> None:
            message = f"{stamp()} --- {text} ---"
            log.write(message + "\n")
            print(message, flush=True)

        def line(data: bytes, partial: bool = False) -> None:
            text = data.decode("utf-8", errors="replace").rstrip("\r")
            if partial:
                text = "[partial] " + text
            log.write(f"{stamp()} {text}\n")
            if stats.line(text):
                print(f"{stamp()} [check] {text}", flush=True)

        event(f"monitor start: {port_name}, {duration:g}s, DTR=RTS=False")
        try:
            while time.monotonic() < deadline:
                if port is None:
                    now = time.monotonic()
                    if now < next_attempt:
                        time.sleep(max(0, min(0.2, next_attempt - now, deadline - now)))
                        continue
                    candidate = serial.Serial(port=None, baudrate=baud, timeout=0.25)
                    candidate.dtr = False
                    candidate.rts = False
                    candidate.port = port_name
                    try:
                        candidate.open()
                    except (serial.SerialException, OSError) as exc:
                        candidate.close()
                        stats.data["open_failures"] += 1
                        if not waiting_reported:
                            event(f"waiting for {port_name}: {exc}")
                            waiting_reported = True
                        next_attempt = time.monotonic() + 2
                        continue
                    port = candidate
                    stats.data["port_opens"] += 1
                    waiting_reported = False
                    event(f"port open: {port_name}")
                try:
                    data = port.read(4096)
                except (serial.SerialException, OSError) as exc:
                    if pending:
                        line(pending, partial=True)
                        pending = b""
                    try:
                        port.close()
                    except OSError:
                        pass
                    port = None
                    stats.data["disconnects"] += 1
                    event(f"port lost: {exc}")
                    waiting_reported = True
                    next_attempt = time.monotonic() + 2
                    continue
                pending += data
                while b"\n" in pending:
                    complete, pending = pending.split(b"\n", 1)
                    line(complete)
                # Do not accumulate unbounded data if the device stops sending LF.
                if len(pending) >= 65536:
                    line(pending, partial=True)
                    pending = b""
        except KeyboardInterrupt:
            outcome = "interrupted"
        except Exception:
            outcome = "error"
            raise
        finally:
            if port is not None:
                try:
                    port.close()
                except OSError:
                    pass
            if pending:
                line(pending, partial=True)
            stats.data.update(ended=stamp(), elapsed_s=round(time.monotonic() - started, 2),
                              outcome=outcome)
            event(f"monitor end: {outcome}")
            with summary_path.open("x", encoding="utf-8") as summary:
                json.dump(stats.data, summary, ensure_ascii=False, indent=2)
                summary.write("\n")
            print(f"Summary: {summary_path}", flush=True)
    return stats.data


def positive_seconds(value: str) -> float:
    seconds = float(value)
    if not 0 < seconds <= 86400:
        raise argparse.ArgumentTypeError("duration must be > 0 and <= 86400 seconds")
    return seconds


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM4")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--duration", type=positive_seconds, default=1800)
    parser.add_argument("--output", type=Path, default=Path(".pio/diagnostics") /
                        ("serial-" + datetime.now().strftime("%Y%m%d-%H%M%S-%f") + ".log"))
    args = parser.parse_args()
    record(args.port, args.duration, args.output, args.baud)


if __name__ == "__main__":
    main()
