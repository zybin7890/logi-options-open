import base64
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Dav(BaseHTTPRequestHandler):
    path_to_file = "/dav/My%20Mouse.bin?version=1"
    auth = "Basic " + base64.b64encode(b"sample-user:sample-password").decode("ascii")

    def log_message(self, *args):
        pass

    def respond(self, status, data=b"", **headers):
        self.send_response(status)
        for name, value in headers.items():
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def accept_request(self):
        if self.path == "/sink":
            self.server.forwarded += 1
            self.respond(200, b"redirect was followed")
            return False
        if self.headers.get("Authorization") != self.auth:
            self.server.challenges += 1
            self.respond(401, **{"WWW-Authenticate": 'Basic realm="fixture"'})
            return False
        if self.path == "/redirect":
            self.respond(307, Location="/sink")
            return False
        return True

    def do_GET(self):
        if not self.accept_request():
            return
        if self.path == self.path_to_file:
            if self.server.file is None:
                self.respond(404)
            else:
                self.respond(200, self.server.file, ETag=self.server.etag)
        elif self.path in ("/weak", "/missing-etag", "/multiple-tags"):
            tags = {"/weak": {"ETag": 'W/"v1"'}, "/multiple-tags": {"ETag": '"v1", "v2"'}}
            self.respond(200, b"package", **tags.get(self.path, {}))
        elif self.path == "/large-stream":
            # No Content-Length: the client must enforce its cap as it reads.
            self.send_response(200)
            self.end_headers()
            try:
                self.wfile.write(b"z" * (4 * 1024 * 1024 + 1))
            except OSError:
                pass
        else:
            self.respond(400)

    def do_PUT(self):
        body = self.rfile.read(int(self.headers["Content-Length"]))
        if not self.accept_request():
            return
        if self.path != self.path_to_file:
            self.server.unexpected_put += 1
            self.respond(400)
            return
        match, absent = self.headers.get("If-Match"), self.headers.get("If-None-Match")
        self.server.conditions.append((match, absent))
        if self.headers.get("Content-Type") != "application/octet-stream":
            self.respond(415)
        elif absent == "*" and match is None and self.server.file is None:
            self.server.file, self.server.etag = body, '"v1"'
            self.respond(201)
        elif match is not None and match == self.server.etag and absent is None:
            self.server.file, self.server.etag = body, '"v2"'
            self.respond(204)
        else:
            self.respond(412)


def main():
    with ThreadingHTTPServer(("127.0.0.1", 0), Dav) as server:
        server.file = server.etag = None
        server.conditions = []
        server.forwarded = server.unexpected_put = server.challenges = 0
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            subprocess.run([sys.argv[1], str(server.server_port)], timeout=25, check=True)
        finally:
            server.shutdown()
            thread.join()
        expected = [(None, "*"), (None, "*"), ('"v1"', None), ('"v1"', None)]
        if (server.conditions != expected or server.forwarded or server.unexpected_put
                or not server.challenges or server.file != bytes([1, 97, 0, 2, 3])):
            raise RuntimeError("WebDAV request sequence or final server state was incorrect")


if __name__ == "__main__":
    main()
