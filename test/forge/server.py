#!/usr/bin/env python3
"""A stand-in for a forge's contents API, over one repository on disk.

This exists so that the file-API strategy can be tested without a forge. It
serves the three endpoint shapes nix-ci-config knows, from the fixture
directory it is pointed at, and answers exactly as the real ones do: a JSON
object with base64 content for a file, an array for a directory, 404 for a path
that is not there.

It also records every path it was asked for, so a test can assert that reading
a configuration cost the requests it should and no others: the whole claim of
the fast path is the number of requests it makes, and nothing else observes it.
"""

import base64
import json
import os
import sys
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs, unquote, urlparse

ROOT = os.path.abspath(sys.argv[1])
PORT = int(sys.argv[2])
REQUEST_LOG = sys.argv[3]
# The revision the fixture is pinned at. A request for any other must not be
# answered from it, or the test would pass for a tool that ignored the
# revision entirely.
REV = sys.argv[4]

_lock = threading.Lock()


def record(path):
    with _lock:
        with open(REQUEST_LOG, "a") as handle:
            handle.write(path + "\n")


def repo_path(relative):
    """The fixture file a repository-relative path names, or None if it escapes."""
    resolved = os.path.abspath(os.path.join(ROOT, relative))
    if resolved != ROOT and not resolved.startswith(ROOT + os.sep):
        return None
    return resolved


def contents_response(relative, with_type):
    resolved = repo_path(relative)
    if resolved is None or not os.path.exists(resolved):
        return None
    if os.path.isdir(resolved):
        if not with_type:
            # GitLab's files endpoint answers only for files, and 404s for a
            # directory exactly as it does for a path that is not there.
            return None
        # The other two report a directory as an array of its entries. Only
        # the shape matters here: a listing is something the tool declines to
        # use.
        return [{"name": name} for name in sorted(os.listdir(resolved))]
    with open(resolved, "rb") as handle:
        encoded = base64.b64encode(handle.read()).decode()
    body = {"encoding": "base64", "content": encoded}
    if with_type:
        # GitLab's files endpoint reports no type; the other two report one.
        body["type"] = "file"
    return body


def tree_response(relative):
    """GitLab's tree endpoint, which is the only way to see a directory there."""
    resolved = repo_path(relative)
    if resolved is None or not os.path.isdir(resolved):
        return []
    return [
        {"name": name, "path": os.path.join(relative, name), "type": "blob"}
        for name in sorted(os.listdir(resolved))
    ]


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        url = urlparse(self.path)
        query = parse_qs(url.query)
        ref = query.get("ref", [None])[0]

        # GitLab's tree endpoint takes the path as a query parameter rather
        # than in the URL path.
        if url.path.startswith("/api/v4/projects/") and url.path.endswith("/repository/tree"):
            relative = query.get("path", [""])[0]
            record("tree:" + relative)
            if ref != REV:
                self.send_error(404)
                return
            self.respond(tree_response(relative))
            return

        relative, with_type = self.route(url.path)
        if relative is None:
            self.send_error(404)
            return

        record(relative)

        if ref != REV:
            # The revision is part of what identifies the file, so a request
            # for one this fixture is not at has no answer.
            self.send_error(404)
            return

        body = contents_response(relative, with_type)
        if body is None:
            self.send_error(404)
            return

        self.respond(body)

    def respond(self, body):
        encoded = json.dumps(body).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def route(self, path):
        """The repository-relative path a request names, and whether to report a type."""
        # GitHub: /repos/<owner>/<repo>/contents/<path>, and the same under
        # /api/v3 for an instance that is not github.com.
        for prefix in ("/api/v3/repos/", "/repos/"):
            if path.startswith(prefix):
                rest = path[len(prefix) :].split("/", 2)
                if len(rest) == 3 and rest[2].startswith("contents/"):
                    return unquote(rest[2][len("contents/") :]), True
                return None, False
        # Forgejo: /api/v1/repos/<owner>/<repo>/contents/<path>
        if path.startswith("/api/v1/repos/"):
            rest = path[len("/api/v1/repos/") :].split("/", 2)
            if len(rest) == 3 and rest[2].startswith("contents/"):
                return unquote(rest[2][len("contents/") :]), True
            return None, False
        # GitLab: /api/v4/projects/<owner%2Frepo>/repository/files/<path>
        if path.startswith("/api/v4/projects/"):
            rest = path[len("/api/v4/projects/") :]
            marker = "/repository/files/"
            if marker in rest:
                return unquote(rest.split(marker, 1)[1]), False
            return None, False
        return None, False


if __name__ == "__main__":
    server = HTTPServer(("127.0.0.1", PORT), Handler)
    # Tell the caller the port, so it never has to guess one that is free.
    print(server.server_address[1], flush=True)
    server.serve_forever()
