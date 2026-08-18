import time
import json
import psutil
import serial
import serial.tools.list_ports
import subprocess
import threading
import os
import sys

BAUD_RATE = 115200

DEFAULT_CONFIG = {
    "TASKMGR":  "taskmgr",
    "NOTEPAD":  "notepad",
    "CALC":     "calc",
    "SPOTIFY":  "start spotify:",
    "CHROME":   "start chrome",
    "SETTINGS": "start ms-settings:",
    "YOUTUBE":  "https://youtube.com"
}

def get_base_dir():
    if getattr(sys, 'frozen', False):
        return os.path.dirname(sys.executable)
    return os.path.dirname(os.path.abspath(__file__))

def load_config():
    config_path = os.path.join(get_base_dir(), "config.json")
    if os.path.exists(config_path):
        try:
            with open(config_path, "r", encoding="utf-8") as f:
                return json.load(f)
        except Exception as e:
            print(f"Error reading config.json: {e}")
            return DEFAULT_CONFIG
    else:
        try:
            with open(config_path, "w", encoding="utf-8") as f:
                json.dump(DEFAULT_CONFIG, f, indent=4)
        except Exception as e:
            print(f"Error writing config.json: {e}")
        return DEFAULT_CONFIG

def auto_detect_esp32_port():
    ports = serial.tools.list_ports.comports()
    for port in ports:
        desc = port.description.lower()
        if any(chip in desc for chip in ["cp210", "ch340", "usb serial", "ftdi"]):
            return port.device
    return ports[0].device if ports else None

class PCControlServer:
    def __init__(self):
        self.ser = None
        self.telemetry_active = False

    def connect(self):
        while True:
            port = auto_detect_esp32_port()
            if port:
                try:
                    print(f"Connecting to ESP32 on {port}...")
                    self.ser = serial.Serial(port, BAUD_RATE, timeout=1)
                    time.sleep(2)
                    print("Connected and ready for ESP32 requests!")
                    return
                except serial.SerialException:
                    pass
            print("ESP32 port not found. Retrying in 3 seconds...")
            time.sleep(3)

    def get_running_user_processes(self):
        """Scans active user applications running on Windows."""
        system_procs = {"svchost.exe", "explorer.exe", "system", "idle", "csrss.exe", 
                        "lsass.exe", "services.exe", "winlogon.exe", "taskhostw.exe", 
                        "conhost.exe", "sihost.exe", "ctfmon.exe", "cmd.exe", "python.exe"}
        active_apps = []
        for proc in psutil.process_iter(['name']):
            try:
                pname = proc.info['name']
                if pname and pname.lower() not in system_procs and pname.endswith('.exe'):
                    clean_name = pname[:-4] # Remove .exe extension
                    if clean_name not in active_apps:
                        active_apps.append(clean_name)
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                continue
        return active_apps[:12] # Return top 12 active processes

    def listen_serial(self):
        while True:
            try:
                if self.ser and self.ser.in_waiting > 0:
                    line = self.ser.readline().decode('utf-8', errors='ignore').strip()
                    
                    # 1. Fetch Startable Apps List
                    if line == "CMD:GET_START_MENU":
                        programs = load_config()
                        keys = list(programs.keys()) + ["< Back"]
                        payload = json.dumps({"start_apps": keys}) + "\n"
                        self.ser.write(payload.encode('utf-8'))
                        print(f"-> Sent Start Apps menu: {keys}")

                    # 2. Fetch Currently Running Processes
                    elif line == "CMD:GET_END_MENU":
                        procs = self.get_running_user_processes() + ["< Back"]
                        payload = json.dumps({"end_apps": procs}) + "\n"
                        self.ser.write(payload.encode('utf-8'))
                        print(f"-> Sent Running Apps menu: {procs}")

                    # 3. Launch Target App
                    elif line.startswith("CMD:LAUNCH:"):
                        app_key = line.split(":")[-1]
                        programs = load_config()
                        if app_key in programs:
                            target = programs[app_key]
                            print(f"-> Launching: {target}")
                            subprocess.Popen(target, shell=True)

                    # 4. Terminate Target Process
                    elif line.startswith("CMD:KILL:"):
                        proc_name = line.split(":")[-1]
                        print(f"-> Terminating process: {proc_name}.exe")
                        subprocess.Popen(f"taskkill /F /IM {proc_name}.exe", shell=True)

                    # 5. Telemetry Control
                    elif line == "CMD:START_TELEMETRY":
                        self.telemetry_active = True
                    elif line == "CMD:STOP_TELEMETRY":
                        self.telemetry_active = False
                    #6. To Lock PC

                    elif line ==  "CMD:LOCK_PC":
                        print("-> Locking PC...")
                        subprocess.Popen("rundll32.exe user32.dll,LockWorkStation", shell=True) 

            except Exception as e:
                print(f"Serial Error: {e}")
                time.sleep(1)

    def send_telemetry(self):
        while True:
            if self.telemetry_active and self.ser:
                try:
                    payload = {
                        "cpu": int(psutil.cpu_percent(interval=None)),
                        "ram": int(psutil.virtual_memory().percent)
                    }
                    self.ser.write((json.dumps(payload) + "\n").encode('utf-8'))
                except Exception as e:
                    print(f"Telemetry Write Error: {e}")
            time.sleep(0.5)

    def run(self):
        self.connect()
        threading.Thread(target=self.listen_serial, daemon=True).start()
        self.send_telemetry()

if __name__ == "__main__":
    server = PCControlServer()
    server.run()