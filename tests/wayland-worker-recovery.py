"""Exercise worker recovery inside the disposable native GNOME/KDE CI session.

Only the HID++ device is simulated. The supervisor, worker, desktop profile
lookup, uinput, compositor transport and receiving Qt window are real.
"""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
from contextlib import contextmanager


def wait_for(description, sample, timeout=12):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = sample()
        if value:
            return value
        time.sleep(0.04)
    raise RuntimeError(f"Timed out waiting for {description}")


def read_state(path):
    try:
        return json.loads(path.read_text())
    except FileNotFoundError:
        return {}


@contextmanager
def child(arguments, env):
    process = subprocess.Popen(arguments, env=env, start_new_session=True)
    try:
        yield process
    finally:
        if process.poll() is None:
            try:
                # Also cleans up a stopped or unresponsive fixture after a failed assertion.
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait(timeout=5)


def proc_state(pid):
    try:
        return Path(f"/proc/{pid}/stat").read_text().rsplit(") ", 1)[1].split()
    except FileNotFoundError:
        return []


def main(directory, receiver, evidence):
    bindir = receiver.parent
    cli = bindir / "logi-cli"
    fixture = bindir / "logi-linux-fixture-daemon"
    config = directory / "mappings.ini"
    config.write_text("[global]\n0053=G\n[org.logi.test.editor]\n0053=E\n")
    receiver_state = directory / "receiver.json"
    env = dict(os.environ, LOGI_OPTIONS_OPEN_CONFIG=str(config))
    # The compositor and its provider share this runtime directory. The session
    # runner created both XDG homes; never enter an existing user's desktop.
    if Path(env["XDG_RUNTIME_DIR"]).parent != Path(env["XDG_CONFIG_HOME"]).parent:
        raise RuntimeError("Expected the session runner's private XDG directories")
    ready_file = Path(env["LOGI_WAYLAND_INPUT_READY"])
    results = {"desktop": env["XDG_CURRENT_DESKTOP"], "physical_mouse": False, "generations": []}
    report = evidence / "worker-recovery.json"

    def record():
        report.write_text(json.dumps(results, indent=2) + "\n")

    def command(*args, timeout=10):
        result = subprocess.run([cli, *args], env=env, capture_output=True, text=True, timeout=timeout)
        if result.returncode:
            raise RuntimeError(f"{args} exited {result.returncode}: {result.stderr} {result.stdout}")
        return result.stdout

    def status():
        return json.loads(command("agent-status"))

    def worker(owner, executable, previous, ready):
        def current():
            state = status()
            pid = state.get("pid", 0)
            if not pid or pid == previous or state.get("ready") is not ready:
                return None
            fields = proc_state(pid)
            if (owner.poll() is not None or state.get("supervisor_pid") != owner.pid or
                    len(fields) < 2 or fields[0] == "Z" or int(fields[1]) != owner.pid or
                    not os.path.samefile(f"/proc/{pid}/exe", executable)):
                raise RuntimeError(f"IPC did not identify this supervisor's mapping worker: {state}")
            return pid
        return wait_for("a live, distinct mapping worker", current)

    def transport_ready():
        try:
            return ready_file.read_text().strip()
        except FileNotFoundError:
            return False

    def deliver(pid, cause, delay):
        wait_for("transport to open the new kernel uinput device", transport_ready)
        def foreground():
            provider = "org.logi.OptionsOpen.Desktop1"
            value = subprocess.check_output(
                ["gdbus", "call", "--session", "--dest", provider, "--object-path",
                 "/org/logi/OptionsOpen/Desktop1", "--method", provider + ".GetForeground"],
                env=env, text=True, timeout=5).strip()
            return value if "org.logi.test.editor" in value.lower() else None
        identity = wait_for("compositor to identify the native Editor profile", foreground)
        before = read_state(receiver_state)
        old = before["key_events"]
        with Path(str(config) + ".events").open("a") as events:
            events.write("buttons 83\nbuttons\n")

        def received():
            snapshot = read_state(receiver_state)
            events = snapshot.get("key_events", [])[len(old):]
            if any(item["key"] == ord("G") for item in events):
                raise RuntimeError(f"Global fallback reached the focused application: {events}")
            expected = [(ord("E"), True), (ord("E"), False)]
            delivered = [(item["key"], item["pressed"]) for item in events]
            if not delivered or delivered == expected[:1]:
                return None
            if (delivered != expected or not snapshot["active"] or not snapshot["focus"] or
                    snapshot["id"] != "org.logi.test.Editor" or not snapshot["platform"].startswith("wayland")):
                raise RuntimeError(f"Unexpected focused Wayland client input: {snapshot}")
            return events

        events = wait_for("profile-specific key press and release in the native client", received)
        results["generations"].append({"cause": cause, "worker_pid": pid, "ready": True,
                                       "ready_ms": delay, "foreground": identity, "client_events": events})
        record()
        print(f"PASS {cause}: worker {pid} ready in {delay} ms; simulated HID 0x53 delivered E down/up", flush=True)

    def stays_stopped(owner):
        if owner.wait(timeout=10) != 0:
            raise RuntimeError("Supervisor returned an error on intentional stop")
        # The production maximum restart delay is two seconds.
        deadline = time.monotonic() + 2.5
        while True:
            if status().get("native") is not False:
                raise RuntimeError("An explicitly stopped worker resurrected")
            if time.monotonic() >= deadline:
                return
            time.sleep(0.15)

    record()
    if status().get("native") is not False:
        raise RuntimeError("Refusing to test with a pre-existing owner")
    command("accept-disclaimer")
    command("check-config", str(config))
    receiver_env = env.copy()
    receiver_env.pop("LOGI_TEST_EVIDENCE", None)
    with child([receiver, "--receiver", "org.logi.test.Editor", receiver_state], receiver_env):
        wait_for("active native Editor window", lambda: read_state(receiver_state).get("focus")
                 and read_state(receiver_state).get("active"))
        ready_file.unlink(missing_ok=True)
        started = time.monotonic()
        with child([fixture, "--wayland-worker", config], env) as owner:
            results["supervisor_pid"] = owner.pid
            pid = worker(owner, fixture, 0, True)
            deliver(pid, "startup", round((time.monotonic() - started) * 1000))
            for sig in (signal.SIGTERM, signal.SIGKILL):
                ready_file.unlink(missing_ok=True)
                started = time.monotonic()
                os.kill(pid, sig)
                pid = worker(owner, fixture, pid, True)
                deliver(pid, sig.name, round((time.monotonic() - started) * 1000))
            if json.loads(command("stop")).get("stopped") is not True:
                raise RuntimeError("Intentional stop was not acknowledged")
            stays_stopped(owner)
            results["stop_did_not_restart"] = True
            record()

    # The production HID enumerator is exercised only on a disposable hosted
    # runner with no hidraw device at all. Local runs never touch a real mouse.
    if (env.get("GITHUB_ACTIONS") == "true" and env.get("RUNNER_ENVIRONMENT") == "github-hosted"
            and not any(Path("/sys/class/hidraw").glob("hidraw*"))):
        with child([cli, "run", config], env) as owner:
            old = worker(owner, cli, 0, False)
            os.kill(old, signal.SIGKILL)
            new = worker(owner, cli, old, False)
            command("stop")
            stays_stopped(owner)
            results["production_cli_without_hardware"] = {"old_worker": old, "new_worker": new,
                                                          "ready": False, "stop_did_not_restart": True}
    else:
        results["production_cli_without_hardware"] = {"skipped": "requires isolated runner without hidraw"}
    record()
    print("PASS native Wayland worker recovery and explicit stop; physical mouse not tested", flush=True)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="loo-wayland-recovery-") as temp:
        main(Path(temp), Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve())
