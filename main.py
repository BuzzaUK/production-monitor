import yaml
import os
import time
import csv
import threading
from datetime import datetime
import RPi.GPIO as GPIO
from collections import defaultdict

CONFIG_FILE = "config.yaml"
DATA_DIR = "data"
CSV_FILE = os.path.join(DATA_DIR, "events.csv")

def load_config():
    with open(CONFIG_FILE, "r") as f:
        return yaml.safe_load(f)

def setup_gpio(machines):
    GPIO.setmode(GPIO.BCM)
    for machine in machines:
        GPIO.setup(machine["gpio"], GPIO.IN, pull_up_down=GPIO.PUD_UP)

def ensure_data_dir():
    if not os.path.exists(DATA_DIR):
        os.makedirs(DATA_DIR)
    if not os.path.exists(CSV_FILE):
        with open(CSV_FILE, "w", newline='') as f:
            writer = csv.writer(f)
            writer.writerow(["timestamp", "machine", "event", "duration_s"])

class MachineMonitor:
    def __init__(self, config):
        self.machines = config["machines"]
        self.status = {m["name"]: True for m in self.machines}  # Assume all running at start
        self.last_change = {m["name"]: datetime.now() for m in self.machines}
        self.lock = threading.Lock()

    def update_status(self):
        while True:
            now = datetime.now()
            for m in self.machines:
                pin = m["gpio"]
                name = m["name"]
                running = GPIO.input(pin) == GPIO.HIGH
                with self.lock:
                    if self.status[name] != running:
                        duration = (now - self.last_change[name]).total_seconds()
                        event = "STOPPED" if not running else "RUNNING"
                        self.log_event(name, event, duration)
                        self.status[name] = running
                        self.last_change[name] = now
            time.sleep(0.1)  # Fast poll for quick response

    def log_event(self, machine, event, duration):
        with open(CSV_FILE, "a", newline='') as f:
            writer = csv.writer(f)
            writer.writerow([datetime.now().isoformat(), machine, event, round(duration, 2)])

def main():
    config = load_config()
    ensure_data_dir()
    setup_gpio(config["machines"])
    monitor = MachineMonitor(config)
    t = threading.Thread(target=monitor.update_status, daemon=True)
    t.start()
    # Start UI and Webserver as subprocesses or threads here
    while True:
        time.sleep(10)  # Keep main thread alive

if __name__ == "__main__":
    main()
