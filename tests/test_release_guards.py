"""回归检查：在 Linux/WSL 下运行 python3 tests/test_release_guards.py，需要 g++。"""

import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ReleaseGuards(unittest.TestCase):
    def test_capacity_pass_condition(self):
        source = (ROOT / "bench/vsomeip_capacity.cpp").read_text()
        condition = re.search(r"const bool pass = (.*?);", source, re.S).group(1)
        # 直接编译容量程序里的判定条件，避免测试副本和实现不同步。
        program = r"""
struct Stats {
    unsigned submitted=100, completed=100, ok=100, errors=0, timeouts=0;
    unsigned bad=0, duplicates=0, peak=4;
};
bool passes(const Stats& st, bool drained=true, unsigned window=4) {
    return CONDITION;
}
int main() {
    Stats good;
    if (!passes(good) || passes(good, false)) return 1;
    Stats all_errors; all_errors.ok=0; all_errors.errors=100;
    if (passes(all_errors)) return 2;
    Stats one_error; one_error.ok=99; one_error.errors=1;
    if (passes(one_error)) return 3;
    Stats timeout; timeout.ok=99; timeout.timeouts=1;
    if (passes(timeout)) return 4;
    Stats empty; empty.submitted=empty.completed=empty.ok=0;
    if (passes(empty)) return 5;
    Stats short_window; short_window.peak=3;
    if (passes(short_window)) return 6;
    Stats bad; bad.bad=1;
    if (passes(bad)) return 7;
    Stats duplicate; duplicate.duplicates=1;
    if (passes(duplicate)) return 8;
    Stats missing; missing.completed=missing.ok=99;
    if (passes(missing)) return 9;
}
""".replace("CONDITION", condition)
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "predicate")
            subprocess.run(
                ["g++", "-std=c++17", "-x", "c++", "-", "-o", binary],
                input=program,
                text=True,
                check=True,
            )
            subprocess.run([binary], check=True)

    def test_checker_exit_is_saved(self):
        for name in ("run_rpc_disconnect_recovery.sh", "run_vsomeip_capacity.sh"):
            script = (ROOT / "scripts" / name).read_text()
            tail = script[script.index("set +e\npython3 ") :]
            for checker_exit, tee_exit in ((0, 0), (1, 0), (2, 0), (0, 1), (1, 1)):
                with self.subTest(script=name, checker=checker_exit, tee=tee_exit):
                    with tempfile.TemporaryDirectory() as directory:
                        # 执行脚本实际使用的收尾代码，模拟检查器和日志写入失败，
                        # 这里不启动 vsomeip 服务。
                        prefix = """set -euo pipefail
root=.; run="$TEST_DIR"; run_dir="$TEST_DIR"; test_exit=0
python3() { printf 'checker output\\n'; return "$CHECKER_EXIT"; }
tee() { command tee "$@"; return "$TEE_EXIT"; }
"""
                        env = dict(
                            os.environ,
                            TEST_DIR=directory,
                            CHECKER_EXIT=str(checker_exit),
                            TEE_EXIT=str(tee_exit),
                        )
                        result = subprocess.run(
                            ["bash", "-c", prefix + tail], env=env, capture_output=True, text=True
                        )
                        saved = (Path(directory) / "check-exit.txt").read_text()
                        self.assertIn(f"check_exit={checker_exit}\n", saved)
                        self.assertIn(f"tee_exit={tee_exit}\n", saved)
                        self.assertEqual(
                            result.returncode == 0, checker_exit == 0 and tee_exit == 0
                        )


if __name__ == "__main__":
    unittest.main()
