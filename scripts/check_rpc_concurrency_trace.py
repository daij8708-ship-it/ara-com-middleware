#!/usr/bin/env python3
"""核对一次多在途请求测试的日志，不启动服务或修改日志。"""

from pathlib import Path
from collections import defaultdict
import re, sys

if len(sys.argv) != 2:
    sys.exit("Usage: python3 scripts/check_rpc_concurrency_trace.py CLIENT_LOG")
events = defaultdict(list)
cases = {}
summaries = []
errors = []
ordered = []
for line in Path(sys.argv[1]).read_text().splitlines():
    f = dict(re.findall(r"(\w+)=(\S+)", line))
    if "[RPC_TRACE]" in line:
        rid = int(f["request_id"])
        events[rid].append(f)
        if rid and f["event"] in ["SUBMIT", "CALLBACK"]:
            ordered.append(f)
    elif line.startswith("CASE "):
        rid = int(f["id"])
        if rid in cases:
            errors.append(f"duplicate CASE id={rid}")
        cases[rid] = f
    elif line.startswith("SUMMARY "):
        summaries.append(f)


def require(ok, message):
    if not ok:
        errors.append(message)


require(len(summaries) == 1, "Expected one SUMMARY")
require(bool(cases), "No CASE records")
window = 0
if len(summaries) == 1:
    summary = summaries[0]
    window = int(summary["window"])
    require(
        int(summary["total"]) == len(cases) == int(summary["requested"]), "request counts mismatch"
    )
    require(set(cases) == set(range(1, int(summary["requested"]) + 1)), "request IDs missing")
    for key in ["failures", "incomplete_before_stop", "watchdog"]:
        require(summary.get(key) == "0", key + " is nonzero")
    require(summary.get("coverage") == "1", "success/timeout/multiple-outstanding coverage missing")
    require(
        int(summary["successes"]) == sum(c["status"] == "0" for c in cases.values()),
        "success count mismatch",
    )
    require(
        int(summary["timeouts"]) == sum(c["status"] == "1" for c in cases.values()),
        "timeout count mismatch",
    )
pending = set()
peak = 0
sessions = {}
ignored_count = 0
response_order = []
# 按日志记录顺序还原在途请求数。SUBMIT 在请求表加锁时记录，
# CALLBACK 在客户端释放在途名额之前记录。
for e in ordered:
    rid = int(e["request_id"])
    if e["event"] == "SUBMIT":
        require(rid not in pending, f"duplicate SUBMIT id={rid}")
        pending.add(rid)
        peak = max(peak, len(pending))
    else:
        require(rid in pending, f"CALLBACK without outstanding request id={rid}")
        pending.discard(rid)
require(not pending, "outstanding trace records remain")
require(1 < peak <= window, f"actual outstanding peak {peak} outside 2..{window}")
for rid, c in sorted(cases.items()):

    def check(ok, msg):
        require(ok, f"id={rid}: {msg}")

    check(
        all(
            c.get(k) == v
            for k, v in {
                "accepted": "1",
                "callbacks": "1",
                "bad_payload": "0",
                "timely_completion": "1",
                "result": "PASS",
            }.items()
        ),
        "CASE failed",
    )
    g = defaultdict(list)
    for e in events[rid]:
        g[e["event"]].append(e)
    check(len(g["SUBMIT"]) == 1 and len(g["CALLBACK"]) == 1, "need one submit and one callback")
    if len(g["SUBMIT"]) != 1 or len(g["CALLBACK"]) != 1:
        continue
    sub = g["SUBMIT"][0]
    cb = g["CALLBACK"][0]
    session = sub["session"]
    check(session not in sessions, "session reused during this bounded run")
    sessions[session] = rid
    check(
        all(e.get("session", session) == session for e in events[rid]), "request/session mismatch"
    )
    check(cb["status"] == c["status"] and cb["count"] == "1", "callback result mismatch")
    check(int(cb["observed_us"]) >= int(sub["observed_us"]), "callback before submit")
    if c["status"] == "0":
        check(
            len(g["RESPONSE_CLAIMED"]) == 1
            and not g["TIMEOUT_CLAIMED"]
            and not g["RESPONSE_IGNORED"],
            "invalid success trace",
        )
        if g["RESPONSE_CLAIMED"]:
            response_order.append((int(g["RESPONSE_CLAIMED"][0]["observed_us"]), rid))
    elif c["status"] == "1":
        check(
            len(g["TIMEOUT_CLAIMED"]) == 1
            and len(g["RESPONSE_IGNORED"]) == 1
            and not g["RESPONSE_CLAIMED"],
            "missing or duplicate timeout/ignored response",
        )
        if len(g["TIMEOUT_CLAIMED"]) == 1:
            t = int(g["TIMEOUT_CLAIMED"][0]["observed_us"])
            check(
                t >= int(sub["deadline_us"]) and int(cb["observed_us"]) >= t,
                "invalid timeout ordering",
            )
        if len(g["RESPONSE_IGNORED"]) == 1:
            ignored_count += 1
            response_order.append((int(g["RESPONSE_IGNORED"][0]["observed_us"]), rid))
    else:
        check(False, "unexpected completion status")
require(not (set(events) - set(cases) - {0}), "trace has unknown request IDs")
ids = [rid for _, rid in sorted(response_order)]
inversions = sum(b < a for a, b in zip(ids, ids[1:]))
print(
    f"CONCURRENCY_CHECK cases={len(cases)} peak_inflight={peak} ignored_responses={ignored_count} response_order_drops={inversions} errors={len(errors)}"
)
for error in errors:
    print("TRACE_FAIL " + error)
if not inversions:
    print("COVERAGE_NOTE no response reordering observed; do not claim out-of-order coverage")
sys.exit(1 if errors else 0)
