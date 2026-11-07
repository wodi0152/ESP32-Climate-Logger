"""ESP32 Climate Logger server (Python standard library only).

Keeps older measurements.csv untouched; new, deduplicated records go to
measurements_v2.csv. Uses device_id + sample_id as the idempotency key.
"""
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Lock
import csv
import json
import math
import os

HOST = "0.0.0.0"
PORT = int(os.environ.get("CLIMATE_PORT", "8080"))
DATA_DIR = Path(os.environ.get("CLIMATE_DATA_DIR", Path(__file__).resolve().parent))
DATA_DIR.mkdir(parents=True, exist_ok=True)
DATA_FILE = DATA_DIR / "measurements_v2.csv"
HEADERS = [
    "received_at_utc", "measured_at_utc", "device_id", "sample_id",
    "temperature_c", "humidity_pct",
]
lock = Lock()


def load_received_keys():
    keys = set()
    if DATA_FILE.exists():
        with DATA_FILE.open("r", newline="", encoding="utf-8") as stream:
            for row in csv.DictReader(stream):
                try:
                    keys.add((row["device_id"], int(row["sample_id"])))
                except (KeyError, ValueError):
                    continue
    return keys


received_keys = load_received_keys()


class Handler(BaseHTTPRequestHandler):
    def reply(self, status, payload):
        data = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path == "/health":
            self.reply(200, {"status": "ok"})
        else:
            self.reply(404, {"error": "not_found"})

    def do_POST(self):
        if self.path != "/api/measurements":
            self.reply(404, {"error": "not_found"})
            return
        try:
            content_length = int(self.headers.get("Content-Length", "0"))
            if not 0 < content_length <= 4096:
                self.reply(413, {"error": "invalid_body_size"})
                return
            payload = json.loads(self.rfile.read(content_length))
            if not isinstance(payload, dict):
                raise ValueError("Not a JSON object")
            device_id = payload["device_id"]
            sample_id = payload["sample_id"]
            temperature = float(payload["temperature"])
            humidity = float(payload["humidity"])
            epoch = payload.get("measured_at_utc")

            if not isinstance(device_id, str) or not 1 <= len(device_id) <= 64:
                raise ValueError("Invalid device ID")
            if isinstance(sample_id, bool) or not isinstance(sample_id, int) or not 1 <= sample_id <= 4294967295:
                raise ValueError("Invalid sample ID")
            if not math.isfinite(temperature) or not -50 <= temperature <= 150:
                raise ValueError("Invalid temperature")
            if not math.isfinite(humidity) or not 0 <= humidity <= 100:
                raise ValueError("Invalid humidity")
            if epoch is None:
                measured_at = ""  # Never invent an offline timestamp.
            else:
                if isinstance(epoch, bool) or not isinstance(epoch, int) or not 1700000000 <= epoch <= 4102444800:
                    raise ValueError("Invalid UTC epoch")
                measured_at = datetime.fromtimestamp(epoch, timezone.utc).isoformat(timespec="seconds")
        except (ValueError, TypeError, KeyError, json.JSONDecodeError):
            self.reply(400, {"error": "invalid_json_or_fields"})
            return

        received_at = datetime.now(timezone.utc).isoformat(timespec="seconds")
        key = (device_id, sample_id)
        try:
            with lock:
                duplicate = key in received_keys
                if not duplicate:
                    is_new = not DATA_FILE.exists() or DATA_FILE.stat().st_size == 0
                    with DATA_FILE.open("a", newline="", encoding="utf-8") as stream:
                        writer = csv.writer(stream)
                        if is_new:
                            writer.writerow(HEADERS)
                        writer.writerow([
                            received_at, measured_at, device_id, sample_id,
                            temperature, humidity,
                        ])
                        stream.flush()
                        os.fsync(stream.fileno())
                    received_keys.add(key)
        except OSError as exc:
            print(f"ERROR saving CSV: {exc}", flush=True)
            self.reply(500, {"error": "storage_failure"})
            return

        print(f"{'DUPLICATE' if duplicate else 'SAVED'} #{sample_id} "
              f"{device_id}: {temperature:.2f} C, {humidity:.2f} %", flush=True)
        self.reply(200, {"ok": True, "duplicate": duplicate, "received_at": received_at})


if __name__ == "__main__":
    print(f"Listening on http://{HOST}:{PORT}")
    print(f"Health check: http://127.0.0.1:{PORT}/health")
    print(f"CSV file: {DATA_FILE}")
    print("Press Ctrl+C to stop")
    try:
        ThreadingHTTPServer((HOST, PORT), Handler).serve_forever()
    except KeyboardInterrupt:
        print("Server stopped")
