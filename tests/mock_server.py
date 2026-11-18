#!/usr/bin/env python3
"""Local stand-in for a job board and the Telegram Bot API, for tests/e2e.sh.

Serves files from a directory (the "job board") and records every
POST /bot<token>/sendMessage in <dir>/telegram.jsonl.

What it answers is driven by files in the directory, so the shell script
can change behaviour between runs:

  telegram_mode   one line:
                    ok           accept (default)
                    down         answer 502
                    unauthorized answer 401
                    no_chat      answer 400 "chat not found" to everything
                    reject_html  answer 400 to messages sent with parse_mode
                    reject_jobs  answer 400 to any message whose text
                                 contains "REJECTME" (formatted or plain)
                    rate_limit:N answer 429 (retry_after 1) to the next N
                                 requests, then accept
  board_status    HTTP status for GET requests other than 200 (e.g. 503)
"""
import json
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = sys.argv[1]


def read(name, default=""):
    try:
        with open(os.path.join(ROOT, name), encoding="utf-8") as f:
            return f.read().strip()
    except FileNotFoundError:
        return default


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def reply(self, status, body, ctype="application/json", headers=None):
        data = body if isinstance(body, bytes) else body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        status = int(read("board_status", "200"))
        if status != 200:
            return self.reply(status, "board unavailable", "text/plain")
        name = os.path.basename(self.path.split("?")[0])
        path = os.path.join(ROOT, name)
        if not name or not os.path.isfile(path):
            return self.reply(404, "not found", "text/plain")
        with open(path, "rb") as f:
            self.reply(200, f.read(), "application/xml")

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length)
        if not self.path.endswith("/sendMessage"):
            return self.reply(404, json.dumps({"ok": False, "error_code": 404, "description": "Not Found"}))
        try:
            msg = json.loads(raw)
        except ValueError:
            return self.reply(400, json.dumps({"ok": False, "error_code": 400, "description": "Bad Request: not JSON"}))

        mode = read("telegram_mode", "ok")
        outcome = "accepted"
        if mode == "down":
            outcome, status, body = "502", 502, "<html>Bad Gateway</html>"
        elif mode == "unauthorized":
            outcome, status = "401", 401
            body = json.dumps({"ok": False, "error_code": 401, "description": "Unauthorized"})
        elif mode == "no_chat":
            outcome, status = "400", 400
            body = json.dumps({"ok": False, "error_code": 400, "description": "Bad Request: chat not found"})
        elif mode == "reject_html" and "parse_mode" in msg:
            outcome, status = "400", 400
            body = json.dumps({"ok": False, "error_code": 400, "description": "Bad Request: can't parse entities"})
        elif mode == "reject_jobs" and "REJECTME" in msg.get("text", ""):
            outcome, status = "400", 400
            body = json.dumps({"ok": False, "error_code": 400, "description": "Bad Request: message rejected"})
        elif mode.startswith("rate_limit:") and int(mode.split(":")[1]) > 0:
            left = int(mode.split(":")[1]) - 1
            with open(os.path.join(ROOT, "telegram_mode"), "w", encoding="utf-8") as f:
                f.write("rate_limit:%d" % left)
            outcome, status = "429", 429
            body = json.dumps({"ok": False, "error_code": 429,
                               "description": "Too Many Requests: retry after 1",
                               "parameters": {"retry_after": 1}})
        else:
            status = 200
            body = json.dumps({"ok": True, "result": {"message_id": 1}})

        with open(os.path.join(ROOT, "telegram.jsonl"), "a", encoding="utf-8") as f:
            f.write(json.dumps({"path": self.path, "outcome": outcome, "message": msg}, ensure_ascii=False) + "\n")
        self.reply(status, body)


def main():
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    with open(os.path.join(ROOT, "port"), "w", encoding="utf-8") as f:
        f.write(str(server.server_address[1]))
    server.serve_forever()


if __name__ == "__main__":
    main()
