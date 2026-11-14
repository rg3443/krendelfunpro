"""Фейковый сервер в формате HTTP API krendelbot — для проверки HttpExecutor.
Запуск: python tests/fake_krendelbot.py <port> <token>
"""
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT, TOKEN = int(sys.argv[1]), sys.argv[2]
FREE = 3
orders = {}


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def reply(self, code, obj):
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def authed(self):
        if self.headers.get("Authorization") != "Bearer " + TOKEN:
            self.reply(401, {"error": "нужен заголовок Authorization: Bearer <token>"})
            return False
        return True

    def do_GET(self):
        if not self.authed():
            return
        if self.path == "/api/status":
            return self.reply(200, {"bots_free": FREE, "bots_total": 3})
        if self.path.startswith("/api/orders/"):
            oid = int(self.path.rsplit("/", 1)[1])
            if oid not in orders:
                return self.reply(404, {"error": f"нет заказа #{oid}"})
            return self.reply(200, orders[oid])
        self.reply(404, {"error": "нет такого пути"})

    def do_POST(self):
        global FREE
        if not self.authed():
            return
        req = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if self.path == "/api/start":
            oid = req["order_id"]
            if oid in orders:
                return self.reply(200, orders[oid])
            n = min(FREE, req["bots"])
            FREE -= n
            orders[oid] = {"order_id": oid, "channel": req["channel"], "tier": req.get("tier"),
                           "bots_requested": req["bots"],
                           "bots_assigned": n, "bots_missing": req["bots"] - n,
                           "status": "running" if n else "waiting", "reason": "",
                           "ends_at": req.get("ends_at") or int(time.time()) + req["hours"] * 3600}
            return self.reply(200, orders[oid])
        if self.path.startswith("/api/orders/") and self.path.endswith("/extend"):
            oid = int(self.path.split("/")[3])
            o = orders.get(oid)
            if not o:
                return self.reply(404, {"error": f"нет заказа #{oid}"})
            if o["status"] in ("completed", "stopped", "failed"):
                return self.reply(409, {"error": f"заказ #{oid} уже завершён ({o['status']})"})
            o["ends_at"] += int(round(req["hours"] * 3600))
            return self.reply(200, o)
        if self.path == "/api/stop":
            o = orders.get(req["order_id"])
            if not o:
                return self.reply(404, {"error": "нет заказа"})
            o["status"], o["reason"] = "stopped", req.get("reason", "")
            return self.reply(200, o)
        if self.path == "/api/_fail":  # служебное: имитация «канал не найден»
            o = orders[req["order_id"]]
            o["status"], o["reason"] = "failed", "канал не найден"
            return self.reply(200, o)
        self.reply(404, {"error": "нет такого пути"})

    def do_DELETE(self):
        if not self.authed():
            return
        oid = int(self.path.rsplit("/", 1)[1])
        if orders.pop(oid, None) is None:
            return self.reply(404, {"error": f"нет заказа #{oid}"})
        self.reply(200, {"order_id": oid, "status": "deleted"})


HTTPServer(("127.0.0.1", PORT), H).serve_forever()
