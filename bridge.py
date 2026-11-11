"""Optional local JSON API and MCP stdio server for Logi Options Open."""
import argparse
import hmac
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = Path(__file__).resolve().parent
WINDOWS = os.name == "nt"
EXE = Path(os.environ.get("LOGI_OPTIONS_OPEN_CLI") or
           (str(ROOT / "logi-cli.exe") if WINDOWS else
            str(ROOT / "logi-cli") if (ROOT / "logi-cli").exists() else shutil.which("logi-cli") or "logi-cli"))
DATA = ROOT if WINDOWS else Path(os.environ.get("XDG_CONFIG_HOME", str(Path.home() / ".config"))) / "logi-options-open"
CONFIG = Path(os.environ.get("LOGI_OPTIONS_OPEN_CONFIG", str(DATA / "mappings.ini")))
LOG = CONFIG.parent / "agent.log"
CREATE_FLAGS = getattr(subprocess, "CREATE_NO_WINDOW", 0) if WINDOWS else 0
SETTINGS = ("dpi", "smartshift", "ratchet", "hires", "invert", "thumb-invert")
LOCK = threading.Lock()
NAME = {"name": "logi-options-open", "version": "0.1.0-alpha.1"}
LEGACY = ("2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05")
MODERN = "2026-07-28"


class BridgeError(Exception):
    def __init__(self, message, status=400):
        super().__init__(message)
        self.status = status


def cli(*args, structured=False):
    try:
        result = subprocess.run(
            [str(EXE), *map(str, args)], cwd=ROOT, capture_output=True,
            encoding="utf-8", errors="replace", timeout=25,
            creationflags=CREATE_FLAGS,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise BridgeError(str(error), 503) from error
    if result.returncode:
        raise BridgeError(result.stderr.strip() or result.stdout.strip()
                          or f"LogiNative exited with code {result.returncode}.", 409)
    return json.loads(result.stdout) if structured else result.stdout.strip()


def launch(*args, log=False):
    try:
        kwargs = {"cwd": ROOT, "creationflags": CREATE_FLAGS,
                  "stdin": subprocess.DEVNULL}
        if log:
            LOG.parent.mkdir(parents=True, exist_ok=True)
            with LOG.open("wb") as output:
                process = subprocess.Popen([str(EXE), *map(str, args)],
                                           stdout=output, stderr=output, **kwargs)
        else:
            process = subprocess.Popen([str(EXE), *map(str, args)],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, **kwargs)
    except OSError as error:
        raise BridgeError(str(error), 503) from error
    return {"pid": process.pid}


def require_keys(args, *keys):
    if type(args) is not dict or args.keys() != set(keys):
        raise BridgeError("Expected parameters: " + ", ".join(keys))


def set_setting(args):
    require_keys(args, "setting", "value")
    setting, value = args["setting"], args["value"]
    if setting not in SETTINGS or type(value) is not int:
        raise BridgeError("Unknown setting or invalid integer.")
    limit = 65535 if setting == "dpi" else 50 if setting == "smartshift" else 1
    if not 0 <= value <= limit:
        raise BridgeError("Value outside the supported range.")
    return {"result": cli("set", setting, value)}


def write_config(args):
    require_keys(args, "text")
    text = args["text"]
    if type(text) is not str or not 0 < len(text.encode("utf-8")) <= 65535:
        raise BridgeError("Expected a UTF-8 mapping file of at most 64 KiB.")
    CONFIG.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=CONFIG.parent, suffix=".ini", delete=False) as temp:
        candidate = Path(temp.name)
        temp.write(text.encode("utf-8"))
    try:
        cli("check-config", candidate)
        os.replace(candidate, CONFIG)
    finally:
        candidate.unlink(missing_ok=True)
    # The replacement is committed. A later notification failure must not imply
    # that saving failed, or roll back a file the running agent may already read.
    try:
        notification = cli("reload", CONFIG, structured=True)
        if (type(notification) is not dict or notification.keys() != {"reload_requested"}
                or type(notification["reload_requested"]) is not bool):
            raise BridgeError("Invalid reload response.", 503)
    except (BridgeError, ValueError) as error:
        return {"saved": True, "reload_requested": False, "reload_error": str(error)}
    return {"saved": True, "reload_requested": notification["reload_requested"]}


def preview_actions(args):
    if not WINDOWS:
        raise BridgeError("Action previews are not supported by the Linux backend yet.", 501)
    require_keys(args, "sequence")
    sequence = args["sequence"]
    if type(sequence) is not str or not sequence or "\r" in sequence or "\n" in sequence:
        raise BridgeError("Expected a single action sequence.")
    with tempfile.NamedTemporaryFile(suffix=".ini", delete=False) as temp:
        path = Path(temp.name)
        temp.write(("[hotkeys]\nCtrl+Alt+F10 = " + sequence + "\n").encode("utf-8"))
    try:
        cli("check-actions", path)
        cli("check-disclaimer")
        process = subprocess.Popen([str(EXE), "play-actions", str(path)], cwd=ROOT,
                                   creationflags=CREATE_FLAGS,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except BaseException:
        path.unlink(missing_ok=True)
        raise

    def cleanup():
        process.wait()
        path.unlink(missing_ok=True)

    threading.Thread(target=cleanup, daemon=True).start()
    return {"delay_ms": 3000}


def perform(operation, args=None):
    args = {} if args is None else args
    with LOCK:
        if operation == "status":
            require_keys(args)
            return cli("status-json", structured=True)
        if operation == "hosts":
            require_keys(args)
            return {"hosts": cli("hosts-json", structured=True)}
        if operation == "agent":
            require_keys(args)
            return cli("agent-status", structured=True)
        if operation == "set":
            return set_setting(args)
        if operation == "switch":
            require_keys(args, "slot")
            if type(args["slot"]) is not int or args["slot"] < 1:
                raise BridgeError("Expected a positive slot number.")
            return {"result": cli("switch-host", args["slot"])}
        if operation == "read-config":
            require_keys(args)
            return {"text": CONFIG.read_text(encoding="utf-8-sig")}
        if operation == "write-config":
            return write_config(args)
        if operation == "start":
            require_keys(args, "takeover")
            if type(args["takeover"]) is not bool:
                raise BridgeError("Expected a boolean takeover flag.")
            if cli("agent-status", structured=True)["native"]:
                raise BridgeError("LogiNative is already active.", 409)
            cli("check-config", CONFIG)
            cli("check-disclaimer")
            return launch("run-preview" if WINDOWS and args["takeover"] else "run", CONFIG, log=True)
        if operation == "stop":
            require_keys(args)
            cli("stop")
            return {"stop_requested": True}
        if operation == "ring":
            require_keys(args)
            if not WINDOWS:
                raise BridgeError("Actions Ring is not supported by the Linux backend yet.", 501)
            return launch("preview-ring", CONFIG)
        if operation == "preview-actions":
            return preview_actions(args)
        if operation == "log":
            require_keys(args)
            return {"text": LOG.read_text(encoding="utf-8", errors="replace")[-4096:]
                    if LOG.exists() else ""}
        raise BridgeError("Unknown operation.", 404)


READS = {"/v1/status": "status", "/v1/hosts": "hosts", "/v1/agent": "agent",
         "/v1/config": "read-config", "/v1/log": "log"}
WRITES = {("PUT", "/v1/setting"): "set", ("PUT", "/v1/config"): "write-config",
          ("POST", "/v1/switch-host"): "switch", ("POST", "/v1/agent/start"): "start",
          ("POST", "/v1/agent/stop"): "stop", ("POST", "/v1/ring/preview"): "ring",
          ("POST", "/v1/actions/preview"): "preview-actions"}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def send_json(self, status, value):
        content = json.dumps(value, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(content)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(content)

    def route(self):
        try:
            origin = self.headers.get("Origin")
            if self.headers.get("Host") != self.server.authority or (
                    origin is not None and origin != "http://" + self.server.authority):
                raise BridgeError("Only the local origin is allowed.", 403)
            auth = self.headers.get("Authorization", "").encode("utf-8")
            if not hmac.compare_digest(auth, b"Bearer " + self.server.token.encode("ascii")):
                raise BridgeError("Local API token required.", 401)
            if self.command == "GET":
                self.send_json(200, perform(READS.get(self.path)))
                return
            operation = WRITES.get((self.command, self.path))
            if operation is None:
                raise BridgeError("Unknown endpoint.", 404)
            size = int(self.headers.get("Content-Length", "0"))
            if not 0 <= size <= 131072:
                raise BridgeError("Request body exceeds the size limit.", 413)
            args = json.loads(self.rfile.read(size)) if size else {}
            self.send_json(200, perform(operation, args))
        except BridgeError as error:
            self.send_json(error.status, {"error": str(error)})
        except (ValueError, UnicodeError):
            self.send_json(400, {"error": "Invalid JSON or request body."})
        except OSError as error:
            self.send_json(503, {"error": str(error)})

    do_GET = do_POST = do_PUT = route


def object_schema(properties=None):
    props = properties or {}
    return {"type": "object", "properties": props, "required": list(props),
            "additionalProperties": False}


def tool(name, operation, description, properties=None, read_only=True):
    return operation, {"name": name, "description": description, "inputSchema": object_schema(properties),
                       "annotations": {"readOnlyHint": read_only, "openWorldHint": False}}


TOOLS = dict((spec["name"], (operation, spec)) for operation, spec in (
    tool("mouse_status", "status", "Read the local Logitech mouse and its current settings."),
    tool("easy_switch_slots", "hosts", "List local Easy-Switch slots and pairing state."),
    tool("agent_status", "agent", "Read the status of the local native and official agents."),
    tool("read_mappings", "read-config", "Read the local mappings.ini."),
    tool("set_mouse_setting", "set", "Set and verify a local mouse setting.",
         {"setting": {"type": "string", "enum": list(SETTINGS)}, "value": {"type": "integer"}}, False),
    tool("save_mappings", "write-config", "Validate and replace the local mappings.ini.",
         {"text": {"type": "string", "maxLength": 65535}}, False),
    tool("start_native_agent", "start", "Start remapping, optionally pausing the official Options+ agent.",
         {"takeover": {"type": "boolean"}}, False),
    tool("stop_native_agent", "stop", "Stop the local agent and restore Options+ if paused.", read_only=False),
    tool("switch_easy_switch_slot", "switch", "Request an already paired host. Requires both agents to be stopped.",
         {"slot": {"type": "integer", "minimum": 1}}, False),
))


def mcp_dispatch(request, allow_write):
    method, params = request.get("method"), request.get("params", {})
    if type(params) is not dict:
        return {"error": {"code": -32602, "message": "Invalid params"}}
    advertised = {"capabilities": {"tools": {}}, "serverInfo": NAME}
    if method == "initialize":
        version = params.get("protocolVersion")
        return {"result": {"protocolVersion": version if version in LEGACY else LEGACY[0], **advertised}}
    meta = params.get("_meta")
    version = meta.get("io.modelcontextprotocol/protocolVersion") if isinstance(meta, dict) else None
    if version is not None and version not in (MODERN, *LEGACY):
        return {"error": {"code": -32022, "message": "Unsupported protocol version",
                          "data": {"supported": [MODERN, *LEGACY], "requested": version}}}
    if method == "server/discover":
        return {"result": {"resultType": "complete", "supportedVersions": [MODERN, *LEGACY],
                           "capabilities": advertised["capabilities"],
                           "_meta": {"io.modelcontextprotocol/serverInfo": NAME}}}
    if method == "ping":
        return {"result": {}}
    if method == "tools/list":
        return {"result": {"tools": [spec for _, spec in TOOLS.values()
                                     if allow_write or spec["annotations"]["readOnlyHint"]]}}
    if method == "tools/call":
        entry = TOOLS.get(params.get("name")) if isinstance(params.get("name"), str) else None
        try:
            if not entry or not (allow_write or entry[1]["annotations"]["readOnlyHint"]):
                raise BridgeError("Tool unavailable in this session.")
            value = perform(entry[0], params.get("arguments", {}))
            return {"result": {"content": [{"type": "text", "text": json.dumps(value, ensure_ascii=False)}]}}
        except (BridgeError, OSError, ValueError) as error:
            return {"result": {"isError": True, "content": [{"type": "text", "text": str(error)}]}}
    return {"error": {"code": -32601, "message": "Method not found"}}


def run_mcp(allow_write):
    for line in sys.stdin.buffer:
        request_id = None
        try:
            request = json.loads(line)
            if type(request) is not dict or request.get("jsonrpc") != "2.0" or "method" not in request:
                response = {"error": {"code": -32600, "message": "Invalid Request"}}
            elif "id" not in request:
                continue
            else:
                request_id = request["id"]
                response = mcp_dispatch(request, allow_write)
                meta = request.get("params", {})
                meta = meta.get("_meta", {}) if isinstance(meta, dict) else {}
                if "result" in response and isinstance(meta, dict) and meta.get("io.modelcontextprotocol/protocolVersion") == MODERN:
                    response["result"]["resultType"] = "complete"
        except (ValueError, UnicodeError):
            response = {"error": {"code": -32700, "message": "Parse error"}}
        payload = {"jsonrpc": "2.0", "id": request_id, **response}
        sys.stdout.buffer.write((json.dumps(payload, ensure_ascii=False) + "\n").encode("utf-8"))
        sys.stdout.buffer.flush()


def run_http(port):
    with ThreadingHTTPServer(("127.0.0.1", port), Handler) as server:
        server.authority = f"127.0.0.1:{server.server_port}"
        server.token = os.environ.get("LOGINATIVE_API_TOKEN") or secrets.token_urlsafe(24)
        if not server.token.isascii() or not all(ch.isalnum() or ch in "_-" for ch in server.token):
            raise SystemExit("LOGINATIVE_API_TOKEN must use ASCII letters, digits, hyphens or underscores.")
        url = f"http://{server.authority}"
        print(f"LogiNative {url}", flush=True)
        if not os.environ.get("LOGINATIVE_API_TOKEN"):
            print(f"Bearer token: {server.token}", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="mode", required=True)
    commands.add_parser("serve").add_argument("--port", type=int, default=47126)
    commands.add_parser("mcp").add_argument("--write", action="store_true")
    args = parser.parse_args()
    if args.mode == "mcp":
        run_mcp(args.write)
    else:
        run_http(args.port)


if __name__ == "__main__":
    main()
