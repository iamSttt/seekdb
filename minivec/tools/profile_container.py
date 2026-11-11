"""Read cgroup v2 counters for a container created by an official Session.

Snapshots preserve per-device io.stat counters and cumulative memory.peak.
They do not reset counters or force writeback. Docker may remove the cgroup
at process exit, so later snapshots can legitimately be unavailable.
"""

from datetime import datetime, timezone
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
from time import perf_counter


def unsigned_integer(text):
    value = int(text.strip())
    if value < 0:
        raise ValueError("negative cgroup counter")
    return value


def flat_counters(text):
    result = {}
    for line in text.splitlines():
        key, value = line.split()
        if key in result:
            raise ValueError("duplicate cgroup counter")
        result[key] = unsigned_integer(value)
    if not result:
        raise ValueError("empty cpu.stat")
    return result


def device_counters(text):
    result = {}
    for line in text.splitlines():
        device, *fields = line.split()
        if not re.fullmatch(r"\d+:\d+", device) or device in result:
            raise ValueError("invalid or repeated io.stat device")
        counters = {}
        for field in fields:
            key, value = field.split("=", 1)
            if key in counters:
                raise ValueError("duplicate io.stat counter")
            counters[key] = unsigned_integer(value)
        result[device] = counters
    return result


def mount_path(cgroup_path, mountinfo):
    group = PurePosixPath(cgroup_path)
    if not group.is_absolute() or ".." in group.parts:
        raise ValueError("invalid process cgroup path")

    def unescape(path):
        return re.sub(r"\\([0-7]{3})", lambda match: chr(int(match[1], 8)), path)

    for line in mountinfo.splitlines():
        before, separator, after = line.partition(" - ")
        fields = before.split()
        filesystem = after.split()
        if not separator or not filesystem or filesystem[0] != "cgroup2" or len(fields) < 5:
            continue
        root = PurePosixPath(unescape(fields[3]))
        try:
            relative = group.relative_to(root)
        except ValueError:
            continue
        mount = Path(unescape(fields[4])).resolve()
        directory = (mount / str(relative)).resolve()
        directory.relative_to(mount)
        return directory
    raise ValueError("process cgroup is outside visible cgroup v2 mounts")


def counter_delta(before, after):
    if before is None or after is None:
        return None
    return {key: value - before[key] if key in before and value >= before[key] else None
            for key, value in after.items()}


class ContainerProfile:
    def __init__(self, session, allow_sudo=False):
        self.session = session
        self.allow_sudo = allow_sudo
        self.snapshots = []
        self.identity = None
        self.started = perf_counter()
        self.endpoint_checked = False
        self.endpoint_error = None

    def read_text(self, path):
        try:
            return path.read_text(encoding="utf-8"), "direct"
        except PermissionError:
            if not self.allow_sudo:
                raise
        run = subprocess.run(["sudo", "-n", "--", "cat", "--", str(path)],
                             capture_output=True, timeout=10, check=True)
        return run.stdout.decode("utf-8"), "sudo"

    def check_local_endpoint(self):
        if self.endpoint_checked:
            if self.endpoint_error:
                raise ValueError(self.endpoint_error)
            return
        self.endpoint_checked = True
        try:
            if sys.platform != "linux":
                raise ValueError("host cgroup profiling requires Linux")
            # A remote daemon's State.Pid could name an unrelated local process.
            # DOCKER_CONTEXT takes precedence over DOCKER_HOST in the Docker CLI.
            host = None if os.environ.get("DOCKER_CONTEXT") else os.environ.get("DOCKER_HOST")
            if not host:
                run = self.session.docker("context", "inspect", "--format",
                                          "{{json .Endpoints.docker.Host}}")
                host = json.loads(run.stdout)
            if not isinstance(host, str) or not host.startswith("unix:///"):
                raise ValueError("Docker endpoint is not a local Unix socket")
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
            self.endpoint_error = str(exc)
            raise

    def discover(self):
        self.check_local_endpoint()
        template = ('{"id":{{json .Id}},"pid":{{json .State.Pid}},'
                    '"running":{{json .State.Running}}}')
        run = self.session.docker("inspect", "--type", "container", "--format", template, self.session.name)
        container = json.loads(run.stdout)
        if type(container) is not dict:
            raise ValueError("Docker returned a non-object container identity")
        ident, pid = container.get("id"), container.get("pid")
        if not isinstance(ident, str) or not re.fullmatch(r"[0-9a-f]{64}", ident):
            raise ValueError("Docker returned an invalid container ID")
        if self.identity is not None and self.identity["id"] != ident:
            self.identity = None
        if container.get("running") is not True or type(pid) is not int or pid <= 0:
            raise ValueError("the benchmark container is not running")
        text, access = self.read_text(Path("/proc") / str(pid) / "cgroup")
        paths = [line[3:] for line in text.splitlines() if line.startswith("0::")]
        if len(paths) != 1:
            raise ValueError("the benchmark process has no unique cgroup v2 membership")
        group = paths[0]
        components = PurePosixPath(group).parts
        if ident not in components and f"docker-{ident}.scope" not in components:
            raise ValueError("process cgroup does not identify this benchmark container")
        mounts, mount_access = self.read_text(Path("/proc/self/mountinfo"))
        self.identity = {"id": ident, "pid": pid, "process_cgroup": group,
                         "directory": str(mount_path(group, mounts)),
                         "membership_access": access, "mountinfo_access": mount_access}
        return self.identity

    def snapshot(self, phase, *, removed=False):
        begin = perf_counter()
        sample = {"phase": phase, "timestamp_utc": datetime.now(timezone.utc).isoformat(),
                  "elapsed_seconds": begin - self.started, "container": None,
                  "memory_peak_bytes": None, "memory_current_bytes": None,
                  "cpu_stat": None, "io_stat": None, "read_access": {}, "errors": {}}
        try:
            if not self.session.image:
                raise ValueError("host container counters are unavailable in --binary mode")
            identity = self.identity if removed else self.discover()
            if identity is None:
                raise ValueError("no previously verified benchmark cgroup is available")
            sample["container"] = {**identity, "removed": removed}
            directory = Path(identity["directory"])
            for filename, key, parse in (("memory.peak", "memory_peak_bytes", unsigned_integer),
                                         ("memory.current", "memory_current_bytes", unsigned_integer),
                                         ("cpu.stat", "cpu_stat", flat_counters),
                                         ("io.stat", "io_stat", device_counters)):
                try:
                    text, access = self.read_text(directory / filename)
                    sample[key] = parse(text)
                    sample["read_access"][key] = access
                except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
                    sample["errors"][key] = str(exc)
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
            sample["errors"]["discovery"] = str(exc)

        previous = self.snapshots[-1] if self.snapshots else None
        sample["delta_from_previous"] = None
        if previous and sample["container"] and previous["container"]:
            if sample["container"]["id"] == previous["container"]["id"]:
                io_before, io_after = previous["io_stat"], sample["io_stat"]
                io_delta = None if io_before is None or io_after is None else {
                    device: counter_delta(io_before.get(device), counters) for device, counters in io_after.items()}
                sample["delta_from_previous"] = {
                    "phase": previous["phase"],
                    "cpu_stat": counter_delta(previous["cpu_stat"], sample["cpu_stat"]),
                    "io_stat": io_delta,
                }
        sample["sample_seconds"] = perf_counter() - begin
        self.snapshots.append(sample)
        return sample

    def report(self):
        return {"snapshots": self.snapshots,
                "scope": "Read-only snapshots of the official Session's own container cgroup v2.",
                "limitations": "memory.peak is cumulative for each container lifetime. io.stat retains device keys; "
                               "stacked devices must not be summed as a physical-I/O score. Missing or unreadable "
                               "metrics and counter deltas are null. Container exit/removal can destroy its cgroup. "
                               "Snapshots do not include guaranteed final writeback and are not official B/M metrics."}
