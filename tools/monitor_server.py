#!/usr/bin/env python3
"""ara-com 运行监测与本机操作面板。

读取演示进程写出的 metrics-*.json 与 *.jsonl，通过本机 HTTP 提供给面板；
同时提供白名单操作：启动/停止 DDS、SOME/IP 演示，运行测试。
只使用 Python 标准库，不依赖 Web 框架；控制接口只监听本机并校验自定义请求头。
"""

import argparse
import csv
import io
import json
import os
import signal
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

LEVELS = {"TRACE": 0, "DEBUG": 1, "INFO": 2, "WARN": 3, "ERROR": 4, "FATAL": 5}
BACKENDS = ("dds", "vsomeip")
TEST_SUITES = (
    "quick", "functional", "unit", "guards", "shm", "vsomeip",
    "recovery", "concurrency", "dds", "asan", "tsan", "capacity", "all",
)
MAX_LOG_BYTES = 4 * 1024 * 1024
MAX_LOG_LIMIT = 2000
MAX_ACTION_BYTES = 64 * 1024
MAX_ACTION_LINES = 80
DEFAULT_LIMIT = 200
ACTION_HEADER = "X-ARA-Monitor"


def utc_now():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def csv_safe(value):
    """避免日志内容被表格软件当成公式执行。"""
    text = str(value)
    return "'" + text if text.startswith(("=", "+", "-", "@")) else text


def load_latest_snapshots(metrics_dir):
    """同一个 role 只保留最新的一份快照，避免重复启动导致累计翻倍。"""
    latest = {}
    if not metrics_dir.is_dir():
        return []
    for path in sorted(metrics_dir.glob("metrics-*.json")):
        try:
            modified = path.stat().st_mtime
            with path.open("r", encoding="utf-8") as handle:
                snapshot = json.load(handle)
        except (OSError, ValueError):
            continue
        role = str(snapshot.get("role", path.stem))
        previous = latest.get(role)
        if previous is None or modified >= previous[0]:
            latest[role] = (modified, snapshot)
    snapshots = []
    for modified, snapshot in latest.values():
        snapshot["snapshot_age_seconds"] = round(max(0.0, time.time() - modified), 1)
        snapshots.append(snapshot)
    snapshots.sort(key=lambda item: (
        str(item.get("backend", "")),
        not str(item.get("role", "")).endswith("-server"),
        str(item.get("role", "")),
    ))
    return snapshots


def filter_snapshots(snapshots, backend):
    if not backend:
        return snapshots
    return [item for item in snapshots if item.get("backend") == backend]


def summarize(snapshots):
    totals = {}
    for snapshot in snapshots:
        counters = snapshot.get("counters", {})
        for key, value in counters.items():
            # 这些指标不能跨进程相加：可用性是或，最大延迟取最大值。
            if key in ("service_available", "rpc_latency_max_us"):
                totals[key] = max(totals.get(key, 0), value)
            elif isinstance(value, (int, float)):
                totals[key] = totals.get(key, 0) + value

    submitted = totals.get("rpc_submitted", 0)
    ok = totals.get("rpc_ok", 0)
    samples = totals.get("rpc_latency_samples", 0)
    latency_total = totals.get("rpc_latency_us_total", 0)
    totals["rpc_ok_rate"] = round(ok / submitted, 4) if submitted else 0.0
    totals["rpc_avg_latency_us"] = round(latency_total / samples) if samples else 0
    totals["active_processes"] = len(snapshots)
    return totals


def tail_json_lines(path):
    try:
        size = path.stat().st_size
    except OSError:
        return []
    entries = []
    try:
        with path.open("rb") as handle:
            if size > MAX_LOG_BYTES:
                handle.seek(size - MAX_LOG_BYTES)
                handle.readline()  # 丢弃被截断的半行
            for raw in handle:
                try:
                    entry = json.loads(raw.decode("utf-8", "replace"))
                except ValueError:
                    continue
                if isinstance(entry, dict):
                    entry["_source"] = path.name
                    entries.append(entry)
    except OSError:
        return []
    return entries


def collect_logs(log_dir, filters, limit, newest_first=True):
    if not log_dir.is_dir():
        return [], 0
    matched = []
    for path in sorted(log_dir.glob("*.jsonl")):
        for entry in tail_json_lines(path):
            component = str(entry.get("component", ""))
            tag = str(entry.get("tag", ""))
            message = str(entry.get("msg", ""))
            level = str(entry.get("level", "INFO")).upper()

            minimum = filters.get("level")
            if minimum and LEVELS.get(level, 2) < LEVELS[minimum]:
                continue
            if filters.get("backend") and entry.get("backend") != filters["backend"]:
                continue
            if filters.get("component") and component != filters["component"]:
                continue
            if filters.get("tag") and tag != filters["tag"]:
                continue
            query = filters.get("q", "").lower()
            if query and query not in message.lower() and query not in tag.lower():
                continue
            matched.append(entry)

    matched.sort(key=lambda item: str(item.get("ts", "")), reverse=newest_first)
    total = len(matched)
    return matched[:limit], total


def tail_text(path, max_bytes=MAX_ACTION_BYTES, lines=MAX_ACTION_LINES):
    """读取动作日志尾部；限制读取量，避免长测试日志拖慢接口。"""
    try:
        size = path.stat().st_size
        with path.open("rb") as handle:
            if size > max_bytes:
                handle.seek(size - max_bytes)
                data = handle.read()
            else:
                data = handle.read()
    except OSError:
        return ""

    text = data.decode("utf-8", "replace")
    if size > max_bytes and "\n" in text:
        text = text.split("\n", 1)[1]
    return "\n".join(text.splitlines()[-lines:])


class ActionError(Exception):
    def __init__(self, status, message):
        super().__init__(message)
        self.status = status
        self.message = message


class JobManager:
    """管理面板发起的演示与测试任务，同一类任务只允许一个实例。"""

    def __init__(self, root, run_dir, build_dir, script_path, defaults):
        self.root = root
        self.run_dir = run_dir
        self.build_dir = build_dir
        self.script_path = script_path
        self.defaults = defaults
        self.lock = threading.Lock()
        self.jobs = {}
        self.demo_jobs = {}
        self.test_job_id = None
        self.counter = 0

    def _spawn_locked(self, kind, args, env_extra, label, backend=None, suite=None):
        self.counter += 1
        stamp = datetime.now().strftime("%H%M%S")
        job_id = "%s-%s-%d" % (kind, stamp, self.counter)
        action_dir = self.run_dir / "actions"
        action_dir.mkdir(parents=True, exist_ok=True)
        log_path = action_dir / (job_id + ".log")

        env = os.environ.copy()
        env.update(env_extra)
        env["ARA_COM_MONITOR_RUN_DIR"] = str(self.run_dir)
        env["ARA_COM_MONITOR_BUILD_DIR"] = str(self.build_dir)
        env["PYTHONUNBUFFERED"] = "1"
        env["NO_COLOR"] = "1"

        handle = log_path.open("ab", buffering=0)
        try:
            process = subprocess.Popen(
                ["bash", str(self.script_path)] + list(args),
                cwd=str(self.run_dir),
                env=env,
                stdout=handle,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
        except OSError:
            handle.close()
            raise

        job = {
            "id": job_id,
            "kind": kind,
            "label": label,
            "backend": backend,
            "suite": suite,
            "status": "running",
            "started_at": utc_now(),
            "ended_at": None,
            "exit_code": None,
            "stop_requested": False,
            "log_path": log_path,
            "process": process,
        }
        self.jobs[job_id] = job
        threading.Thread(target=self._watch, args=(job_id, process, handle), daemon=True).start()
        return job_id

    def _watch(self, job_id, process, handle):
        exit_code = process.wait()
        try:
            handle.close()
        except OSError:
            pass

        with self.lock:
            job = self.jobs.get(job_id)
            if not job:
                return
            job["exit_code"] = exit_code
            job["ended_at"] = utc_now()
            if job["stop_requested"]:
                job["status"] = "stopped"
            else:
                job["status"] = "succeeded" if exit_code == 0 else "failed"
            if job["kind"] == "demo" and self.demo_jobs.get(job["backend"]) == job_id:
                self.demo_jobs.pop(job["backend"], None)
            if job["kind"] == "tests" and self.test_job_id == job_id:
                self.test_job_id = None

    def _running_locked(self, job_id):
        job = self.jobs.get(job_id)
        return bool(job and job["status"] == "running")

    def start_demo(self, backend, duration, requests, clients):
        with self.lock:
            if backend not in BACKENDS:
                raise ActionError(400, "未知后端")
            current = self.demo_jobs.get(backend)
            if current and self._running_locked(current):
                raise ActionError(409, "%s 演示已经在运行" % backend)
            if self.test_job_id and self._running_locked(self.test_job_id):
                raise ActionError(409, "测试正在运行，请先停止测试")

            label = "DDS 演示" if backend == "dds" else "SOME/IP 演示"
            job_id = self._spawn_locked(
                "demo",
                ["demo", backend],
                {
                    "ARA_COM_MONITOR_DURATION": str(duration),
                    "ARA_COM_MONITOR_REQUESTS": str(requests),
                    "ARA_COM_MONITOR_CLIENTS": str(clients),
                },
                label,
                backend=backend,
            )
            self.demo_jobs[backend] = job_id
            return self.snapshot_locked()

    def stop_demo(self, backend):
        with self.lock:
            job_id = self.demo_jobs.get(backend)
            if not job_id or not self._running_locked(job_id):
                raise ActionError(409, "该后端当前没有运行中的演示")
        return self.stop_job(job_id)

    def start_tests(self, suite):
        if suite not in TEST_SUITES:
            raise ActionError(400, "未知测试档位")
        with self.lock:
            if self.test_job_id and self._running_locked(self.test_job_id):
                raise ActionError(409, "测试已经在运行")
            for backend in BACKENDS:
                job_id = self.demo_jobs.get(backend)
                if job_id and self._running_locked(job_id):
                    raise ActionError(409, "请先停止正在运行的演示")

            labels = {
                "quick": "快速回归测试",
                "functional": "功能回归测试",
                "unit": "单元测试",
                "guards": "判定逻辑回归",
                "shm": "共享内存测试",
                "vsomeip": "SOME/IP 端到端测试",
                "recovery": "服务停止与恢复测试",
                "concurrency": "并发压测",
                "dds": "DDS 回归测试",
                "asan": "ASan/UBSan 测试",
                "tsan": "TSan 测试",
                "capacity": "容量/极限测试",
                "all": "全部测试",
            }
            job_id = self._spawn_locked(
                "tests",
                ["tests", suite],
                {},
                labels[suite],
                suite=suite,
            )
            self.test_job_id = job_id
            return self.snapshot_locked()

    def stop_job(self, job_id):
        with self.lock:
            job = self.jobs.get(job_id)
            if not job:
                raise ActionError(404, "任务不存在")
            if job["status"] != "running":
                return self.snapshot_locked()
            job["stop_requested"] = True
            process = job["process"]

        self._terminate_group(process)
        return self.snapshot()

    def _terminate_group(self, process):
        try:
            if hasattr(os, "killpg"):
                os.killpg(os.getpgid(process.pid), signal.SIGTERM)
            else:
                process.terminate()
        except ProcessLookupError:
            return
        timer = threading.Timer(5.0, self._kill_group, args=(process.pid,))
        timer.daemon = True
        timer.start()

    @staticmethod
    def _kill_group(pid):
        try:
            if hasattr(os, "killpg"):
                os.killpg(os.getpgid(pid), signal.SIGKILL)
            else:
                os.kill(pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass

    def snapshot_locked(self):
        jobs = []
        recent = sorted(self.jobs.values(), key=lambda item: item["started_at"], reverse=True)
        for index, job in enumerate(recent[:12]):
            # 只给运行中的任务和最近一条任务读取日志，避免每秒重复读大量历史文件。
            include_log = job["status"] == "running" or index == 0
            jobs.append({
                "id": job["id"],
                "kind": job["kind"],
                "label": job["label"],
                "backend": job["backend"],
                "suite": job["suite"],
                "status": job["status"],
                "started_at": job["started_at"],
                "ended_at": job["ended_at"],
                "exit_code": job["exit_code"],
                "log": tail_text(job["log_path"]) if include_log else "",
            })

        demos = {}
        for backend in BACKENDS:
            job_id = self.demo_jobs.get(backend)
            demos[backend] = job_id if job_id and self._running_locked(job_id) else None
        test_id = self.test_job_id
        tests_running = bool(test_id and self._running_locked(test_id))

        return {
            "ok": True,
            "demos": demos,
            "tests_running": tests_running,
            "test_job": test_id if tests_running else None,
            "defaults": dict(self.defaults),
            "jobs": jobs,
        }

    def snapshot(self):
        with self.lock:
            return self.snapshot_locked()

    def shutdown(self):
        with self.lock:
            running = [job["id"] for job in self.jobs.values() if job["status"] == "running"]
        for job_id in running:
            try:
                self.stop_job(job_id)
            except ActionError:
                continue
        deadline = time.time() + 6.0
        while time.time() < deadline:
            with self.lock:
                if not any(job["status"] == "running" for job in self.jobs.values()):
                    return
            time.sleep(0.1)


class MonitorHandler(BaseHTTPRequestHandler):
    server_version = "ara-com-monitor/1.1"

    def log_message(self, fmt, *args):
        # 只把访问日志写到标准错误，由启动脚本落盘，避免污染页面接口。
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path in ("/", "/index.html"):
            self.serve_html()
        elif parsed.path == "/api/state":
            self.serve_state(parsed)
        elif parsed.path == "/api/logs":
            self.serve_logs(parsed, as_csv=False)
        elif parsed.path == "/api/export.csv":
            self.serve_logs(parsed, as_csv=True)
        elif parsed.path == "/api/actions":
            self.send_json(200, self.server.jobs.snapshot())
        elif parsed.path == "/api/health":
            self.send_json(200, {"ok": True, "time": utc_now()})
        else:
            self.send_json(404, {"ok": False, "error": "not_found"})

    def do_POST(self):
        parsed = urlparse(self.path)
        if parsed.path != "/api/actions":
            self.send_json(404, {"ok": False, "error": "not_found"})
            return
        if self.headers.get(ACTION_HEADER) != "1":
            self.send_json(403, {"ok": False, "error": "missing_action_header"})
            return
        if "application/json" not in (self.headers.get("Content-Type") or ""):
            self.send_json(415, {"ok": False, "error": "content_type"})
            return
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            length = 0
        if length <= 0 or length > MAX_ACTION_BYTES:
            self.send_json(400, {"ok": False, "error": "bad_length"})
            return

        try:
            payload = json.loads(self.rfile.read(length).decode("utf-8"))
            if not isinstance(payload, dict):
                raise ValueError("payload")
            result = self.server.handle_action(payload)
            self.send_json(200, result)
        except ValueError:
            self.send_json(400, {"ok": False, "error": "bad_json"})
        except ActionError as error:
            self.send_json(error.status, {"ok": False, "error": error.message})
        except OSError as error:
            self.send_json(500, {"ok": False, "error": str(error)})

    def do_OPTIONS(self):
        self.send_json(405, {"ok": False, "error": "method_not_allowed"})

    def serve_html(self):
        try:
            content = self.server.html_path.read_bytes()
        except OSError:
            self.send_json(500, {"ok": False, "error": "html_missing"})
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(content)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(content)

    def serve_state(self, parsed):
        query = parse_qs(parsed.query)
        backend = query.get("backend", [""])[0]
        if backend not in ("",) + BACKENDS:
            backend = ""
        snapshots = filter_snapshots(load_latest_snapshots(self.server.metrics_dir), backend)
        payload = {
            "ok": True,
            "backend": backend,
            "run_dir": str(self.server.run_dir),
            "server_time": utc_now(),
            "processes": snapshots,
            "summary": summarize(snapshots),
        }
        self.send_json(200, payload)

    def serve_logs(self, parsed, as_csv):
        query = parse_qs(parsed.query)
        backend = query.get("backend", [""])[0]
        if backend not in ("",) + BACKENDS:
            backend = ""
        filters = {
            "level": query.get("level", [""])[0].upper(),
            "backend": backend,
            "component": query.get("component", [""])[0],
            "tag": query.get("tag", [""])[0],
            "q": query.get("q", [""])[0],
        }
        if filters["level"] not in LEVELS:
            filters["level"] = ""
        try:
            limit = int(query.get("limit", [DEFAULT_LIMIT])[0])
        except ValueError:
            limit = DEFAULT_LIMIT
        limit = max(1, min(MAX_LOG_LIMIT, limit))

        entries, total = collect_logs(self.server.log_dir, filters, limit)
        if not as_csv:
            self.send_json(200, {"ok": True, "total": total, "entries": entries})
            return

        buffer = io.StringIO()
        writer = csv.writer(buffer)
        writer.writerow(["ts", "level", "backend", "component", "tag", "pid", "msg"])
        for entry in entries:
            writer.writerow([
                csv_safe(entry.get("ts", "")),
                csv_safe(entry.get("level", "")),
                csv_safe(entry.get("backend", "")),
                csv_safe(entry.get("component", "")),
                csv_safe(entry.get("tag", "")),
                csv_safe(entry.get("pid", "")),
                csv_safe(entry.get("msg", "")),
            ])
        content = buffer.getvalue().encode("utf-8-sig")
        self.send_response(200)
        self.send_header("Content-Type", "text/csv; charset=utf-8")
        self.send_header("Content-Disposition", 'attachment; filename="ara-com-logs.csv"')
        self.send_header("Content-Length", str(len(content)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(content)

    def send_json(self, status, payload):
        content = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(content)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(content)


class MonitorServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, handler, root, run_dir, metrics_dir, log_dir, html_path,
                 build_dir, script_path, defaults):
        super().__init__(address, handler)
        self.root = root
        self.run_dir = run_dir
        self.metrics_dir = metrics_dir
        self.log_dir = log_dir
        self.html_path = html_path
        self.build_dir = build_dir
        self.jobs = JobManager(root, run_dir, build_dir, script_path, defaults)

    @staticmethod
    def _require_int(payload, name, minimum, maximum):
        if name not in payload:
            raise ActionError(400, "缺少参数 %s" % name)
        try:
            value = int(payload[name])
        except (TypeError, ValueError):
            raise ActionError(400, "参数 %s 必须是整数" % name)
        if value < minimum or value > maximum:
            raise ActionError(400, "参数 %s 超出范围" % name)
        return value

    def handle_action(self, payload):
        action = payload.get("action")
        if action == "start_demo":
            backend = payload.get("backend")
            if backend not in BACKENDS:
                raise ActionError(400, "未知后端")
            duration = self._require_int(payload, "duration", 5, 3600)
            requests = self._require_int(payload, "requests", 1, 10000)
            clients = self._require_int(payload, "clients", 1, 8)
            return self.jobs.start_demo(backend, duration, requests, clients)
        if action == "stop_demo":
            backend = payload.get("backend")
            if backend not in BACKENDS:
                raise ActionError(400, "未知后端")
            return self.jobs.stop_demo(backend)
        if action == "start_tests":
            suite = payload.get("suite")
            if suite not in TEST_SUITES:
                raise ActionError(400, "未知测试档位")
            return self.jobs.start_tests(suite)
        if action == "stop_job":
            job_id = payload.get("job_id")
            if not isinstance(job_id, str) or not job_id:
                raise ActionError(400, "缺少 job_id")
            return self.jobs.stop_job(job_id)
        raise ActionError(400, "未知动作")


def parse_args():
    parser = argparse.ArgumentParser(description="ara-com 运行监测与本机操作面板")
    parser.add_argument("--run-dir", required=True, help="运行目录，默认包含 metrics/ 与 logs/")
    parser.add_argument("--metrics-dir", help="指标快照目录，默认 <run-dir>/metrics")
    parser.add_argument("--log-dir", help="JSONL 日志目录，默认 <run-dir>/logs")
    parser.add_argument("--root", help="项目根目录，默认 tools/ 的上一级")
    parser.add_argument("--build-dir", help="演示构建目录，默认 <root>/build-monitor")
    parser.add_argument("--host", default="127.0.0.1", help="监听地址，默认仅本机")
    parser.add_argument("--port", type=int, default=8099, help="监听端口")
    parser.add_argument("--autostart", choices=("none",) + BACKENDS, default="none",
                        help="面板启动后自动启动的演示，默认 none")
    parser.add_argument("--duration", type=int, default=120, help="自动启动的演示时长（秒）")
    parser.add_argument("--requests", type=int, default=300, help="DDS 每个客户端的 RPC 次数")
    parser.add_argument("--clients", type=int, default=4, help="DDS 每批客户端数")
    return parser.parse_args()


def main():
    args = parse_args()
    run_dir = Path(args.run_dir).resolve()
    root = Path(args.root).resolve() if args.root else Path(__file__).resolve().parent.parent
    build_dir = Path(args.build_dir).resolve() if args.build_dir else root / "build-monitor"
    metrics_dir = Path(args.metrics_dir).resolve() if args.metrics_dir else run_dir / "metrics"
    log_dir = Path(args.log_dir).resolve() if args.log_dir else run_dir / "logs"
    html_path = root / "monitor" / "index.html"
    script_path = root / "scripts" / "monitor_action.sh"
    if not html_path.is_file():
        print("缺少面板文件: %s" % html_path, file=sys.stderr)
        return 2
    if not script_path.is_file():
        print("缺少动作脚本: %s" % script_path, file=sys.stderr)
        return 2

    defaults = {
        "duration": args.duration,
        "requests": args.requests,
        "clients": args.clients,
    }
    try:
        server = MonitorServer((args.host, args.port), MonitorHandler,
                               root, run_dir, metrics_dir, log_dir, html_path,
                               build_dir, script_path, defaults)
    except OSError as error:
        print("无法监听 %s:%d: %s" % (args.host, args.port, error), file=sys.stderr)
        return 2

    def stop_server(signum, frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, stop_server)

    if args.autostart != "none":
        try:
            server.jobs.start_demo(args.autostart, args.duration, args.requests, args.clients)
        except ActionError as error:
            print("自动启动演示失败: %s" % error.message, file=sys.stderr)

    print("MONITOR_READY http://%s:%d/ run_dir=%s" % (args.host, args.port, run_dir), flush=True)
    try:
        server.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        pass
    finally:
        server.jobs.shutdown()
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
