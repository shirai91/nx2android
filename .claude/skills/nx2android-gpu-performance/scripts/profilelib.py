"""Device adapter for identity-checked nx2android profiling. Python standard library only."""
from __future__ import annotations

import hashlib
import io
import json
import math
from pathlib import Path, PurePosixPath
import re
import shlex
import statistics
import struct
import subprocess
import tarfile
import time
import zipfile


class ProfileError(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise ProfileError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def stats(values):
    values = sorted(values)
    return dict(n=len(values), mean=statistics.mean(values), median=statistics.median(values),
                p90=values[int((len(values)-1)*.9)], minimum=values[0], maximum=values[-1]) if values else None


def checked_output(repo, output):
    repo, output = Path(repo).resolve(), Path(output).resolve()
    base = repo / ".gamedump"
    require(output.is_relative_to(base) and len(output.relative_to(base).parts) >= 2,
            "Output must be inside <repo>/.gamedump/<App>/<run>; no game artifacts in the skill")
    return output


def load_config(path):
    path = Path(path).resolve()
    config = json.loads(path.read_text(encoding="utf-8"))
    require(config.get("schema") == 1, "Unsupported session schema")
    config["_path"] = str(path)
    require(re.fullmatch(r"[A-Za-z0-9_]+(?:\.[A-Za-z0-9_]+)+", config["package"]), "Invalid package")
    require(config.get("serial"), "An explicit adb serial is required")
    checked_output(config["repo"], config["output"])
    return config


def parse_env(blob):
    return dict(line.strip().split("=", 1) for line in blob.decode("utf-8").splitlines()
                if "=" in line and not line.lstrip().startswith("#"))


def symbol_offsets(text):
    result = {}
    for name, size in (("g_frame_gpu", 512), ("g_frame_gpu_of", 512),
                       ("g_tex_frame", 8), ("g_vkst", 104)):
        hits = [int(a, 16) for a, n in re.findall(
            r"^([0-9a-fA-F]+) ([0-9a-fA-F]+) [bBdD] " + name + r"$", text, re.M)
                if int(n, 16) == size]
        require(len(hits) == 1, f"Missing/ambiguous {name} with expected ABI size {size}; use matching debug ELF")
        result[name] = hits[0]
    return result


def mapped_span(ranges, start, size):
    position = start
    for a, b in sorted(ranges):
        if a <= position < b:
            position = min(start + size, b)
        if position == start + size:
            return True
    return False


def decode_history(blob, gpu_offset, tag_offset):
    require(gpu_offset >= 0 and tag_offset >= 0 and
            max(gpu_offset, tag_offset) + 512 <= len(blob), "Truncated history export")
    times = struct.unpack_from("<64Q", blob, gpu_offset)
    tags = struct.unpack_from("<64Q", blob, tag_offset)
    valid = {tag-1: ns/1e6 for i, (tag, ns) in enumerate(zip(tags, times))
             if tag and (tag-1) % 64 == i and 0 < ns < 1_000_000_000}
    upper = max(valid, default=0)-8
    completed = {f: t for f, t in valid.items() if f <= upper}
    require(len(completed) >= 32, "Insufficient valid completed history; reject stale/partial exports")
    return completed, max(valid)


def delta_ticks(a, b, bits, period):
    require(1 <= bits <= 64 and math.isfinite(period) and period > 0, "Invalid timestamp properties")
    value = ((b-a) & ((1 << bits)-1))*period/1e6
    require(value <= 5000, "Reject ambiguous/stale timestamp span over five seconds")
    return value


def archive_hashes(blob):
    result = {}
    with tarfile.open(fileobj=io.BytesIO(blob)) as archive:
        for entry in archive:
            name = PurePosixPath(entry.name)
            require(not name.is_absolute() and ".." not in name.parts and "\\" not in entry.name,
                    "Unsafe save archive path")
            require(entry.isfile() or entry.isdir(), "Unexpected link/device in save archive")
            if entry.isfile():
                require(entry.name not in result, "Duplicate save archive member")
                result[entry.name] = sha(archive.extractfile(entry).read())
    require(result, "Empty committed-save archive; configure save_roots explicitly")
    return result


class Device:
    def __init__(self, config):
        self.config = config
        self.adb = [config["adb"], "-s", config["serial"]]
        self.package = config["package"]
        self.files = config.get("files", "/sdcard/Android/data/" + self.package + "/files")
        require(re.fullmatch(r"/[A-Za-z0-9_./-]+", self.files) and ".." not in PurePosixPath(self.files).parts,
                "Invalid remote files directory")

    def call(self, *args, timeout=40):
        result = subprocess.run(self.adb + list(args), capture_output=True, timeout=timeout)
        require(result.returncode == 0, f"adb {args[0]} failed: {result.stderr.decode(errors='replace')[-500:]}")
        return result.stdout

    def shell(self, command):
        return self.call("shell", command).decode(errors="replace").replace("\r", "").strip()

    def pid(self):
        value = self.shell("pidof " + self.package)
        require(value.isdigit(), "Expected one running process; do not restart automatically")
        return value

    def focused(self):
        require(re.search(r"\b" + re.escape(self.package) + r"/", self.shell("dumpsys window | grep mCurrentFocus")), "App is not focused")

    def remote_hash(self, path):
        words = self.shell("sha256sum " + shlex.quote(path)).split()
        require(words and re.fullmatch("[0-9a-f]{64}", words[0]), "Remote hash unavailable: " + path)
        return words[0]

    def blob(self, name):
        return self.call("exec-out", "cat", self.files + "/" + name)

    def fingerprint(self):
        apk = self.shell("pm path " + self.package).splitlines()
        require(len(apk) == 1 and apk[0].startswith("package:"), "Split/missing APK; explicit adapter required")
        return self.remote_hash(apk[0][8:])

    def verify(self):
        require(self.fingerprint() == self.config["apk_sha256"], "Installed APK changed; rerun init with its matching ELF")
        require(sha(Path(self.config["elf"]).read_bytes()) == self.config["elf_sha256"], "Local ELF changed")
        for name, expected in self.config.get("assets", {}).items():
            require(self.remote_hash(self.files + "/" + name) == expected, "Asset/config changed: " + name)

    def control(self, values):
        require(values, "No controls supplied")
        lines = []
        for key, value in values.items():
            require(re.fullmatch(r"DM_(?:VK|NVN)_[A-Z0-9_]+", key), "Unsupported control key")
            value = str(value)
            require("\n" not in value and "\r" not in value and "\0" not in value, "Invalid control value")
            lines.append(f"env {key}={value}\n")
        self.shell("printf %s " + shlex.quote("".join(lines)) + " >> " + shlex.quote(self.files + "/ctl.txt"))

    def sensors(self):
        result = {}
        for path in self.config.get("sensors", []):
            require(path.startswith("/sys/") and ".." not in PurePosixPath(path).parts and
                    re.fullmatch(r"/[A-Za-z0-9_./-]+", path), "Invalid sensor path")
            result[path] = self.shell("cat " + shlex.quote(path) + " 2>/dev/null || true") or "unavailable"
        for path, value in self.config.get("required_sensors", {}).items():
            require(path in result and result[path] == str(value), "Required sensor/cap changed: " + path)
        return result

    def log_offset(self):
        return int(self.shell("wc -c < " + shlex.quote(self.files + "/log.txt")))

    def log_since(self, offset):
        return self.shell(f"tail -c +{offset+1} " + shlex.quote(self.files + "/log.txt"))


class History:
    """Matching-symbol adapter for the current 64-slot/13-counter debug ABI."""
    def __init__(self, device, output):
        self.device, self.output = device, Path(output)
        self.output.mkdir(parents=True, exist_ok=True)
        device.verify()
        device.focused()
        self.pid = device.pid()
        text = subprocess.check_output([device.config["llvm_nm"], "-S", device.config["elf"]]).decode()
        offsets = symbol_offsets(text)
        maps = device.call("shell", "run-as", device.package, "cat", "/proc/" + self.pid + "/maps").decode()
        (self.output / "maps.txt").write_text(maps, encoding="utf-8")
        bases, ranges = [], []
        for line in maps.splitlines():
            fields = line.split()
            lo, hi = [int(x, 16) for x in fields[0].split("-")]
            if "r" in fields[1]:
                ranges.append((lo, hi))
            if fields[-1].endswith("/" + device.config.get("host_name", "libdmhost.so")):
                bases.append(lo-int(fields[2], 16))
        require(bases, "Matching host mapping not found")
        self.address = {k: min(bases)+v for k, v in offsets.items()}
        self.lo = min(self.address["g_frame_gpu"], self.address["g_frame_gpu_of"])
        self.hi = max(self.address["g_frame_gpu"], self.address["g_frame_gpu_of"])+512
        require(self.hi-self.lo <= 8192, "Unexpected history layout")
        for start, size in ((self.lo, self.hi-self.lo), (self.address["g_tex_frame"], 8),
                            (self.address["g_vkst"], 104)):
            require(mapped_span(ranges, start, size), "Unmapped counter range")
        self.remote = device.files + "/diagnostics/perf-" + str(time.time_ns())
        device.call("shell", "mkdir", "-p", self.remote)
        self.index = 0
        write_json(self.output / "reader.json", dict(pid=self.pid, addresses=self.address,
                   elf_sha256=device.config["elf_sha256"], abi="64 history slots,13 replay uint64 counters"))

    def sample(self):
        d = self.device
        require(d.pid() == self.pid, "Process restarted; counter addresses invalid")
        paths = [("history", self.lo, self.hi-self.lo), ("frame", self.address["g_tex_frame"], 8),
                 ("replay", self.address["g_vkst"], 104)]
        lines, exports = [], []
        for label, addr, size in paths:
            path = self.remote + f"/{self.index}-{label}.bin"
            lines.append(f"memdump {addr:x} {size} {path}\n")
            exports.append((label, path, size))
        d.shell("printf %s " + shlex.quote("".join(lines)) + " >> " + shlex.quote(d.files + "/ctl.txt"))
        for _ in range(30):
            time.sleep(.03)
            if d.shell("wc -c < " + shlex.quote(exports[-1][1]) + " 2>/dev/null || true") == "104":
                break
        else:
            raise ProfileError("DM_CTL memory export incomplete; verify debug build and matching identity")
        blobs = {}
        for label, path, size in exports:
            data = d.call("exec-out", "cat", path)
            require(len(data) == size, "Incomplete memory export")
            (self.output / f"{self.index:04d}-{label}.bin").write_bytes(data)
            blobs[label] = data
        require(d.pid() == self.pid, "Process changed during export")
        self.index += 1
        completed, newest = decode_history(blobs["history"], self.address["g_frame_gpu"]-self.lo,
                                          self.address["g_frame_gpu_of"]-self.lo)
        replay = struct.unpack("<13Q", blobs["replay"])
        return dict(frame=struct.unpack("<Q", blobs["frame"])[0], completed=completed,
                    newest=newest, replay_ns=replay[9], descriptor_ns=replay[10],
                    pipeline_ns=replay[11], draws=replay[0])


def initialize(args):
    output = checked_output(args.repo, args.output)
    output.mkdir(parents=True, exist_ok=True)
    require(not (output / "session.json").exists(), "Use a new output folder; do not overwrite a session")
    config = dict(schema=1, adb=str(Path(args.adb).resolve()), serial=args.serial, package=args.package,
                  repo=str(Path(args.repo).resolve()), output=str(output), elf=str(Path(args.elf).resolve()),
                  llvm_nm=str(Path(args.nm).resolve()), host_name=args.host_name,
                  sensors=args.sensor or [], required_sensors={}, assets={}, live_controls=[],
                  save_roots=args.save_root or ["files/save"])
    d = Device(config)
    apk = d.shell("pm path " + d.package).splitlines()
    require(len(apk) == 1, "Split/missing APK not supported by this adapter")
    data = d.call("exec-out", "cat", apk[0].removeprefix("package:"))
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        entry = "lib/arm64-v8a/" + args.host_name
        require(z.read(entry) == Path(config["elf"]).read_bytes(), "ELF does not match installed APK host")
    config.update(apk_sha256=sha(data), elf_sha256=sha(Path(config["elf"]).read_bytes()))
    for name in dict.fromkeys(["env.txt", "settings.txt"] + (args.asset or [])):
        path = PurePosixPath(name)
        require(not path.is_absolute() and ".." not in path.parts and "\\" not in name, "Invalid asset name")
        config["assets"][name] = d.remote_hash(d.files + "/" + name)
    config["restore_controls"] = {k: v for k, v in parse_env(d.blob("env.txt")).items()
                                  if k.startswith(("DM_VK_", "DM_NVN_"))}
    (output / "original-env.txt").write_bytes(d.blob("env.txt"))
    (output / "original-settings.txt").write_bytes(d.blob("settings.txt"))
    write_json(output / "session.json", config)
    return config
