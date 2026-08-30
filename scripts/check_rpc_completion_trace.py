#!/usr/bin/env python3
"""检查单次客户端运行的日志，不启动测试，也不修改原日志。"""

import sys, re
from pathlib import Path
from collections import defaultdict

if len(sys.argv) != 2:
    sys.exit("Usage: python3 scripts/check_rpc_completion_trace.py CLIENT_STDOUT_LOG")
text = Path(sys.argv[1]).read_text()
events = defaultdict(list)
cases = {}
errors = []
for line in text.splitlines():
    if "[RPC_TRACE]" in line:
        fields = dict(re.findall(r"(\w+)=(\S+)", line))
        events[int(fields["request_id"])].append(fields)
    if line.startswith("CASE "):
        fields = dict(re.findall(r"(\w+)=(\S+)", line))
        rid = int(fields["id"])
        if rid in cases:
            errors.append(f"duplicate CASE id={rid}; do not merge runs")
        cases[rid] = fields
summaries = [line for line in text.splitlines() if line.startswith("SUMMARY ")]
if len(summaries) != 1:
    errors.append("Expected exactly one SUMMARY from one completed run")
if not cases:
    errors.append("No CASE results")
if summaries:
    summary = dict(re.findall(r"(\w+)=(\S+)", summaries[-1]))
    if int(summary["total"]) != len(cases):
        errors.append("CASE count differs from SUMMARY")
    if summary.get("failures") != "0" or summary.get("incomplete_before_stop") != "0":
        errors.append("Test summary failed")
late_verified = 0
for rid, c in sorted(cases.items()):

    def require(condition, message):
        if not condition:
            errors.append(f"id={rid}: {message}")

    require(
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
        "CASE did not pass",
    )
    grouped = defaultdict(list)
    for e in events[rid]:
        grouped[e["event"]].append(e)
    submit = grouped["SUBMIT"]
    callback = grouped["CALLBACK"]
    require(len(submit) == 1, "need exactly one SUBMIT")
    require(len(callback) == 1, "need exactly one CALLBACK trace")
    if len(submit) != 1 or len(callback) != 1:
        continue
    submit = submit[0]
    callback = callback[0]
    require(
        callback["count"] == "1" and callback["status"] == c["status"],
        "callback trace differs from CASE",
    )
    session = submit["session"]
    require(all(e.get("session", session) == session for e in events[rid]), "session mismatch")
    require(
        int(callback["observed_us"]) >= int(submit["observed_us"]),
        "callback precedes submission record",
    )
    if c["status"] == "0":
        require(
            len(grouped["RESPONSE_CLAIMED"]) == 1, "successful request needs one claimed response"
        )
        require(
            not grouped["TIMEOUT_CLAIMED"] and not grouped["RESPONSE_IGNORED"],
            "unexpected timeout or duplicate response",
        )
    elif c["status"] == "1":
        timeout = grouped["TIMEOUT_CLAIMED"]
        ignored = grouped["RESPONSE_IGNORED"]
        require(len(timeout) == 1, "timeout request needs one timeout claim")
        require(len(ignored) == 1, "timeout request needs one observed ignored response")
        require(not grouped["RESPONSE_CLAIMED"], "response and timeout both claimed completion")
        if len(timeout) == 1:
            require(
                int(timeout[0]["observed_us"]) >= int(submit["deadline_us"]),
                "timeout before deadline",
            )
            require(
                int(callback["observed_us"]) >= int(timeout[0]["observed_us"]),
                "callback before timeout claim",
            )
        if c["name"] == "late" and len(ignored) == 1:
            ordered = int(ignored[0]["observed_us"]) > int(callback["observed_us"])
            require(ordered, "late response was not observed after timeout callback")
            if ordered:
                late_verified += 1
                print(
                    f"LATE_CHAIN id={rid} session={session} timeout_callback_us={callback['observed_us']} response_received_us={ignored[0]['observed_us']} callbacks=1"
                )
    else:
        require(False, "unexpected completion status")
extra_ids = set(events) - set(cases) - {0}
if extra_ids:
    errors.append("trace IDs without CASE: " + str(sorted(extra_ids)))
if sum(c["name"] == "late" for c in cases.values()) != 10:
    errors.append("Expected 10 late cases")
print(f"TRACE_CHECK cases={len(cases)} late_chains={late_verified} errors={len(errors)}")
for e in errors:
    print("TRACE_FAIL " + e)
sys.exit(1 if errors else 0)
