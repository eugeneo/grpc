# Copyright 2025 gRPC authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
import os
import platform
import subprocess
import sys
import tempfile
import time

_DEBUGGER_TIMEOUT_S = 60


def _should_use_lldb() -> bool:
    if platform.system() == "Darwin":
        use_lldb = bool(os.environ.get("USE_LLDB_ON_DARWIN"))
        if not use_lldb:
            sys.stderr.write(
                "LLDB is recommended for use on Mac.\n"
                + 'Run "sudo DevToolsSecurity -enable" and set USE_LLDB_ON_DARWIN'
                + " environment variable to use lldb\n"
            )
        return use_lldb
    return False


def print_backtraces(pid: int):
    if _should_use_lldb():  # macOS
        debugger_name = "lldb"
        cmd = [
            "lldb",
            "--no-lldbinit",
            "-b",  # Batch mode: exit after commands are executed
            "-o",
            "process attach --pid {}".format(pid),
            "-o",
            "thread backtrace all",  # Dumps backtraces of all threads
            "-o",
            "quit",
        ]
    else:
        debugger_name = "gdb"
        cmd = [
            "gdb",
            "-ex",
            "set confirm off",
            "-ex",
            "attach {}".format(pid),
            "-ex",
            "thread apply all bt",
            "-ex",
            "quit",
        ]
    streams = tuple(tempfile.TemporaryFile() for _ in range(2))
    sys.stderr.write(f"Invoking {debugger_name}\n")
    sys.stderr.flush()
    process = subprocess.Popen(cmd, stdout=streams[0], stderr=streams[1])
    try:
        process.wait(timeout=_DEBUGGER_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        sys.stderr.write(
            "{} stacktrace generation timed out.\n".format(debugger_name)
        )
    finally:
        for stream_name, stream in zip(("STDOUT", "STDERR"), streams):
            stream.seek(0)
            sys.stderr.write(
                "{} {}:\n{}\n".format(
                    debugger_name, stream_name, stream.read().decode("ascii")
                )
            )
            stream.close()
        sys.stderr.flush()


def monitor_for_crash(pid: int):
    if _should_use_lldb():
        debugger_name = "lldb"
        cmd = [
            "lldb",
            "--no-lldbinit",
            "-b",  # Batch mode: exit after commands are executed
            "-o",
            f"process attach --pid {pid}",
            "-o",
            "settings set target.process.follow-fork-mode child",
            "-o",
            "continue",
            "-o",
            "bt",  # Backtrace of the current thread
            "-o",
            "quit",
        ]
    else:
        debugger_name = "gdb"
        cmd = [
            "gdb",
            "-ex",
            "set confirm off",
            "-ex",
            f"attach {pid}",
            "-ex",
            "set follow-fork-mode child",
            "-ex",
            "continue",
            "-ex",
            "bt",
        ]

    streams = tuple(tempfile.TemporaryFile() for _ in range(2))
    sys.stderr.write(f"Invoking {debugger_name}\n")
    sys.stderr.flush()
    # It seems `monitor_for_crash` is designed to keep the debugger attached
    # and let it print to stderr directly, without explicitly waiting for
    # the debugger to exit (as `print_backtraces` does).
    # This might be for continuous monitoring.
    process = subprocess.Popen(cmd, stdout=sys.stderr, stderr=sys.stderr)
    time.sleep(5)  # Give the debugger some time to attach and run
