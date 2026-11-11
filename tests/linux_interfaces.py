"""Real CLI/HTTP/MCP subprocesses against the production daemon loop.

The fixture daemon substitutes only the physical HID device and input sink.
This validates process boundaries, persistence and acknowledgement, NOT hardware.
"""
import json
import os
from pathlib import Path
import selectors
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request


def require(value, message):
    if not value:
        raise AssertionError(message)


def line_with_timeout(process, seconds=10):
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        if not selector.select(seconds):
            raise AssertionError("Process did not announce readiness")
        return process.stdout.readline().strip()


def main():
    cli, fixture, bridge = map(lambda value: str(Path(value).resolve()), sys.argv[1:4])
    children = []
    with tempfile.TemporaryDirectory(prefix="loo-interfaces-") as directory:
        root = Path(directory)
        config = root / "mappings.ini"
        config.write_text("[global]\n0053 = MouseBack\nthumb.left = VolumeDown\n", encoding="utf-8")
        env = dict(os.environ, XDG_CONFIG_HOME=str(root / "config"),
                   XDG_RUNTIME_DIR=str(root), LOGI_OPTIONS_OPEN_CONFIG=str(config),
                   LOGI_OPTIONS_OPEN_CLI=cli, LOGINATIVE_API_TOKEN="integration_only_not_a_secret")

        def command(*args, ok=True):
            result = subprocess.run([cli, *args], env=env, text=True,
                                    capture_output=True, timeout=15)
            if ok:
                require(result.returncode == 0, f"CLI {args} failed: {result.stderr} {result.stdout}")
            return result

        def status():
            return json.loads(command("agent-status").stdout)

        def wait(predicate, message, seconds=10):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                if predicate():
                    return
                time.sleep(0.05)
            raise AssertionError(message)

        def spawn(args):
            child = subprocess.Popen(args, env=env, text=True,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            children.append(child)
            return child

        try:
            require(command("check-disclaimer", ok=False).returncode != 0, "Fresh user unexpectedly accepted")
            command("accept-disclaimer")
            command("check-disclaimer")
            command("check-config", str(config))
            daemon = spawn([fixture, str(config)])
            wait(lambda: status().get("ready") is True, "Fixture daemon never became ready")
            command("set", "dpi", "1600")
            require(json.loads(command("status-json").stdout)["settings"]["dpi"] == 1600,
                    "CLI write was not observed by a separate CLI process")
            print("PASS: isolated consent persisted; separate CLI processes share daemon settings", flush=True)

            server = spawn([sys.executable, bridge, "serve", "--port", "0"])
            announced = line_with_timeout(server)
            address = announced[announced.index("http://"):]

            def http(method, path, data=None, authenticated=True):
                headers = {"Content-Type": "application/json"}
                if authenticated:
                    headers["Authorization"] = "Bearer " + env["LOGINATIVE_API_TOKEN"]
                request = urllib.request.Request(address + path,
                    data=None if data is None else json.dumps(data).encode(), headers=headers, method=method)
                try:
                    with urllib.request.urlopen(request, timeout=15) as response:
                        return response.status, json.load(response)
                except urllib.error.HTTPError as error:
                    return error.code, json.load(error)

            require(http("GET", "/v1/status", authenticated=False)[0] == 401,
                    "HTTP accepted an unauthenticated device request")
            code, result = http("PUT", "/v1/setting", {"setting": "dpi", "value": 2400})
            require(code == 200, f"HTTP setting rejected: {result}")
            require(http("GET", "/v1/status")[1]["settings"]["dpi"] == 2400,
                    "HTTP readback did not observe committed setting")
            before = config.read_text(encoding="utf-8")
            require(http("PUT", "/v1/config", {"text": "[global]\n0053=UnknownKey\n"})[0] >= 400,
                    "Invalid mapping unexpectedly accepted")
            require(config.read_text(encoding="utf-8") == before, "Rejected config overwrote valid mappings")
            changed = "[global]\n0053 = MouseForward\nthumb.left = VolumeDown\n"
            code, result = http("PUT", "/v1/config", {"text": changed})
            require(code == 200 and result.get("saved") and result.get("reload_requested"),
                    f"HTTP save/reload was not acknowledged: {result}")
            require(config.read_text(encoding="utf-8") == changed, "Persisted config differs")
            print("PASS: real HTTP authentication, device readback, invalid config rejection and reload", flush=True)

            def mcp(requests, write):
                args = [sys.executable, bridge, "mcp"] + (["--write"] if write else [])
                completed = subprocess.run(args, env=env, text=True, capture_output=True, timeout=20,
                    input="".join(json.dumps(item) + "\n" for item in requests))
                require(completed.returncode == 0, completed.stderr)
                return [json.loads(line) for line in completed.stdout.splitlines()]

            call = {"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {
                "name": "set_mouse_setting", "arguments": {"setting": "dpi", "value": 3200}}}
            require(mcp([call], False)[0]["result"].get("isError") is True,
                    "Read-only MCP session performed a mutation")
            require(http("GET", "/v1/status")[1]["settings"]["dpi"] == 2400,
                    "Rejected MCP write changed the setting")
            reply = mcp([call, {"jsonrpc": "2.0", "id": 2, "method": "tools/call",
                "params": {"name": "mouse_status", "arguments": {}}}], True)
            require(not reply[0]["result"].get("isError"), f"MCP write failed: {reply[0]}")
            require(json.loads(reply[1]["result"]["content"][0]["text"])["settings"]["dpi"] == 3200,
                    "MCP follow-up read did not observe mutation")
            print("PASS: real MCP stdio write gating and CLI/HTTP/MCP shared state", flush=True)

            code, result = http("POST", "/v1/actions/preview", {"sequence": "Ctrl+C;Wait(40);Ctrl+V"})
            require(code == 200 and result.get("accepted") is True,
                    f"Action was not actually accepted by daemon: {result}")
            wait(lambda: status().get("pending_actions") == 0, "Action queue did not drain")
            require(not status().get("last_action_error"), "Queued action reported an execution error")
            require(http("POST", "/v1/agent/stop", {})[0] == 200, "Stop request failed")
            require(daemon.wait(timeout=10) == 0, "Daemon failed orderly shutdown")
            require(status().get("native") is False, "Stopped daemon still advertised active")
            print("PASS: queued action completion and graceful stop across real process boundaries", flush=True)
        finally:
            for child in reversed(children):
                if child.poll() is None:
                    child.terminate()
                try:
                    child.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.communicate()


if __name__ == "__main__":
    main()
