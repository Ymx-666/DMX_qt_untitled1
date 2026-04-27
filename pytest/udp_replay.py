import argparse
import datetime as _dt
import re
import socket
import time
from pathlib import Path
from typing import Iterable, List, Optional, Tuple


_LINE_RE = re.compile(r"^\[(?P<ts>[^\]]+)\][^:]*:\s*(?P<payload>.*)$")


def _parse_ts(ts_text: str) -> Optional[_dt.datetime]:
    try:
        return _dt.datetime.strptime(ts_text.strip(), "%Y-%m-%d %H:%M:%S")
    except ValueError:
        return None


def load_log_payloads(log_path: Path) -> Tuple[List[str], Optional[float]]:
    if not log_path.exists():
        raise FileNotFoundError(str(log_path))

    payloads: List[str] = []
    timestamps: List[_dt.datetime] = []

    for raw in log_path.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = raw.strip()
        if not line:
            continue
        m = _LINE_RE.match(line)
        if not m:
            continue
        payload = (m.group("payload") or "").strip()
        if not payload:
            continue
        payloads.append(payload)
        ts = _parse_ts(m.group("ts") or "")
        if ts is not None:
            timestamps.append(ts)

    if not payloads:
        raise ValueError(f"No payload lines parsed from {log_path}")

    interval_s: Optional[float] = None
    if len(timestamps) >= 2:
        duration = (max(timestamps) - min(timestamps)).total_seconds()
        if duration > 0:
            interval_s = duration / float(len(payloads))

    return payloads, interval_s


def choose_ports(payload: str, port_8001: int, port_8002: int, port_8003: int) -> List[int]:
    head = payload.split(";", 1)[0].strip().lower()
    if head == "bw":
        return [port_8001, port_8002]
    if head == "rgb":
        return [port_8001, port_8003]
    return [port_8001]


def replay(
    payloads: Iterable[str],
    dst_ip: str,
    port_8001: int,
    port_8002: int,
    port_8003: int,
    only_port: Optional[int],
    interval_s: float,
    loop: bool,
    dry_run: bool,
    print_like_log: bool,
    print_ports: bool,
    print_ports_every: int,
    log_sender_ip: str,
    log_sender_port: int,
) -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    try:
        seq = 0
        while True:
            next_send = time.perf_counter()
            for payload in payloads:
                ports = [only_port] if only_port is not None else choose_ports(payload, port_8001, port_8002, port_8003)
                data = payload.encode("utf-8", errors="ignore")
                if not dry_run:
                    for p in ports:
                        sock.sendto(data, (dst_ip, p))
                seq += 1
                if print_ports and (print_ports_every <= 1 or (seq % print_ports_every) == 1):
                    head = payload.split(";", 1)[0].strip()
                    print(f"[send#{seq}] head={head} ports={ports}", flush=True)
                if print_like_log:
                    now = _dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                    print(
                        f"[{now}] 来自 ('{log_sender_ip}', {log_sender_port}) 的数据: {payload}",
                        flush=True,
                    )

                next_send += interval_s
                wait = next_send - time.perf_counter()
                if wait > 0:
                    time.sleep(wait)
            if not loop:
                return
    finally:
        sock.close()


def main() -> int:
    here = Path(__file__).resolve()
    default_log = here.parents[1] / "udp_log.txt"

    ap = argparse.ArgumentParser()
    ap.add_argument("--log", default=str(default_log))
    ap.add_argument("--dst-ip", default="192.168.4.21")
    ap.add_argument("--port-8001", type=int, default=8001)
    ap.add_argument("--port-8002", type=int, default=8002)
    ap.add_argument("--port-8003", type=int, default=8003)
    ap.add_argument("--interval-ms", type=float, default=500.0)
    ap.add_argument("--loop", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--print-like-log", action="store_true")
    ap.add_argument("--print-ports", action="store_true")
    ap.add_argument("--print-ports-every", type=int, default=20)
    ap.add_argument("--only-port", type=int, choices=[8001, 8002, 8003])
    ap.add_argument("--log-sender-ip", default="192.168.4.1")
    ap.add_argument("--log-sender-port", type=int, default=56717)
    args = ap.parse_args()

    payloads, inferred_interval_s = load_log_payloads(Path(args.log))
    interval_s = args.interval_ms / 1000.0 if args.interval_ms > 0 else (inferred_interval_s or 0.5)
    if interval_s <= 0:
        interval_s = 0.5

    replay(
        payloads=payloads,
        dst_ip=args.dst_ip,
        port_8001=args.port_8001,
        port_8002=args.port_8002,
        port_8003=args.port_8003,
        only_port=args.only_port,
        interval_s=interval_s,
        loop=args.loop,
        dry_run=args.dry_run,
        print_like_log=args.print_like_log,
        print_ports=args.print_ports,
        print_ports_every=args.print_ports_every,
        log_sender_ip=args.log_sender_ip,
        log_sender_port=args.log_sender_port,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
