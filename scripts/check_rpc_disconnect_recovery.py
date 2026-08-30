#!/usr/bin/env python3
from pathlib import Path
import re, sys

if len(sys.argv) != 2:
    sys.exit("Usage: checker CLIENT_LOG")
t = Path(sys.argv[1]).read_text()
errors = []


def need(x, m):
    if not x:
        errors.append(m)


cases = [dict(re.findall(r"(\w+)=(\S+)", x)) for x in t.splitlines() if x.startswith("CASE ")]
sums = [dict(re.findall(r"(\w+)=(\S+)", x)) for x in t.splitlines() if x.startswith("SUMMARY ")]
need(len(cases) == 16, "need 16 cases")
need(len(sums) == 1, "need one summary")
if len(cases) == 16:
    need([int(x["id"]) for x in cases] == list(range(1, 17)), "IDs missing")
    need(
        all(
            x["accepted"] == "1"
            and x["callbacks"] == "1"
            and x["bad"] == "0"
            and x["result"] == "PASS"
            for x in cases
        ),
        "case failure",
    )
if len(sums) == 1:
    s = sums[0]
    for k, v in {
        "submitted": "16",
        "once": "16",
        "failures": "0",
        "recovered": "1",
        "coverage": "1",
    }.items():
        need(s.get(k) == v, f"{k} != {v}")
    need(int(s.get("unavailable_events", "0")) >= 1, "no unavailable event")
    need(int(s.get("available_events", "0")) >= 2, "no second available event")
need("READY_TO_KILL submitted=16" in t, "no kill marker")
need("READY_TO_RESTART" in t, "no restart marker")
print(f"DISCONNECT_CHECK cases={len(cases)} errors={len(errors)}")
for e in errors:
    print("CHECK_FAIL " + e)
sys.exit(1 if errors else 0)
