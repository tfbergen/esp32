#!/usr/bin/env python3
# reads current values (V) from 2 TDS sensors connected to a Raspberry pico 2 
# Saves the measurements in a csv file
#
import argparse, csv, re, sys, time
from datetime import datetime
import serial
from serial.tools import list_ports

LINE_RE = re.compile(r"TDS1\(V\):\s*([0-9.]+)\s*TDS2\(V\):\s*([0-9.]+)")

def guess_port():
    # Try to pick a reasonable default on Linux
    candidates = [p.device for p in list_ports.comports() if "ttyACM" in p.device or "ttyUSB" in p.device]
    return candidates[0] if candidates else None

def main():
    ap = argparse.ArgumentParser(description="Read Pico TDS voltages and log to CSV.")
    ap.add_argument("--port", "-p", default=guess_port(), help="Serial port (e.g., /dev/ttyACM0).")
    ap.add_argument("--baud", "-b", type=int, default=115200, help="Baud rate.")
    ap.add_argument("--csv", default="tds_log.csv", help="CSV file to append.")
    ap.add_argument("--print", dest="do_print", action="store_true", help="Print readings as they arrive.")
    args = ap.parse_args()

    if not args.port:
        print("No serial port found. Plug the Pico in and try --port /dev/ttyACM0", file=sys.stderr)
        sys.exit(1)

    print(f"Opening {args.port} @ {args.baud} ...")
    ser = serial.Serial(args.port, args.baud, timeout=2)
    # Give USB-CDC a moment
    time.sleep(0.5)
    # Clear any buffered partial line
    ser.reset_input_buffer()

    # Open CSV
    csv_exists = False
    try:
        csv_exists = open(args.csv, "r")
        csv_exists.close()
        csv_exists = True
    except:
        csv_exists = False

    with open(args.csv, "a", newline="") as f:
        writer = csv.writer(f)
        if not csv_exists:
            writer.writerow(["timestamp_iso", "tds1_v", "tds2_v"])
        try:
            while True:
                line = ser.readline().decode(errors="ignore").strip()
                if not line:
                    continue
                m = LINE_RE.search(line)
                if m:
                    v1 = float(m.group(1))
                    v2 = float(m.group(2))
                    ts = datetime.utcnow().isoformat()
                    writer.writerow([ts, f"{v1:.6f}", f"{v2:.6f}"])
                    f.flush()
                    if args.do_print:
                        print(f"{ts}  TDS1={v1:.3f} V  TDS2={v2:.3f} V")
                # If your sketch prints other lines, ignore them silently
        except KeyboardInterrupt:
            print("\nStopped by user.")
        finally:
            ser.close()

if __name__ == "__main__":
    main()
