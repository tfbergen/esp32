#!/usr/bin/env python3
import time
import math
from smbus2 import SMBus
import RPi.GPIO as GPIO

# ---------- GPIO for MUX select lines (BCM numbering) ----------
S0, S1, S2, S3 = 17, 27, 22, 23

def mux_select(ch: int):
    if not 0 <= ch <= 15:
        raise ValueError("MUX channel must be 0..15")
    GPIO.output(S0, (ch >> 0) & 1)
    GPIO.output(S1, (ch >> 1) & 1)
    GPIO.output(S2, (ch >> 2) & 1)
    GPIO.output(S3, (ch >> 3) & 1)

# ---------- I2C addresses ----------
BUS = 1
AD593x = 0x0D
ADG715 = 0x48

# ---------- Timing ----------
MCLK_HZ = 1_000_000
AD5934_CLK_DIV = 16

# ---------- Sweep settings ----------
START_HZ = 1_000
INC_HZ   = 1_000
STOP_HZ  = 6_000
NUM_INCS = (STOP_HZ - START_HZ) // INC_HZ

SETTLING_CYCLES = 200
SWEEPS_PER_CHANNEL = 3

# ---------- AD5934 registers ----------
REG_CTRL_H   = 0x80
REG_CTRL_L   = 0x81
REG_START_F  = 0x82
REG_INC_F    = 0x85
REG_NUM_INC  = 0x88
REG_SETTLE   = 0x8A
REG_STATUS   = 0x8F
REG_REAL     = 0x94
REG_IMAG     = 0x96

STAT_VALID_DATA = 0x02

CMD_STANDBY   = 0xB0
CMD_INIT      = 0x10
CMD_START_SW  = 0x20
CMD_INC_FREQ  = 0x30

PGA_X1 = 0x01

def freq_to_code(freq_hz: int) -> int:
    dds_clk = MCLK_HZ / AD5934_CLK_DIV
    return int(freq_hz * (1 << 27) / dds_clk) & 0xFFFFFF

def w8(bus, addr, reg, val):
    bus.write_byte_data(addr, reg, val & 0xFF)

def r8(bus, addr, reg) -> int:
    return bus.read_byte_data(addr, reg) & 0xFF

def w16(bus, addr, reg, val):
    w8(bus, addr, reg, (val >> 8) & 0xFF)
    w8(bus, addr, reg + 1, val & 0xFF)

def w24(bus, addr, reg, val):
    w8(bus, addr, reg, (val >> 16) & 0xFF)
    w8(bus, addr, reg + 1, (val >> 8) & 0xFF)
    w8(bus, addr, reg + 2, val & 0xFF)

def s16(hi, lo) -> int:
    v = (hi << 8) | lo
    return v - 0x10000 if (v & 0x8000) else v

def set_cmd(bus, cmd):
    w8(bus, AD593x, REG_CTRL_H, (cmd | PGA_X1) & 0xFF)
    w8(bus, AD593x, REG_CTRL_L, 0x08)

def wait_valid(bus, timeout_s=2.0):
    t0 = time.time()
    while True:
        st = r8(bus, AD593x, REG_STATUS)
        if st & STAT_VALID_DATA:
            return st
        if time.time() - t0 > timeout_s:
            return None
        time.sleep(0.005)

def read_complex(bus):
    b = bus.read_i2c_block_data(AD593x, REG_REAL, 4)
    real = s16(b[0], b[1])
    imag = s16(b[2], b[3])
    return real, imag

def configure_sweep(bus):
    w8(bus, ADG715, 0x00, 0x81)
    time.sleep(0.02)

    set_cmd(bus, CMD_STANDBY)
    w24(bus, AD593x, REG_START_F, freq_to_code(START_HZ))
    w24(bus, AD593x, REG_INC_F,   freq_to_code(INC_HZ))
    w16(bus, AD593x, REG_NUM_INC, NUM_INCS)
    w16(bus, AD593x, REG_SETTLE,  SETTLING_CYCLES)

def run_single_sweep(bus):
    configure_sweep(bus)

    set_cmd(bus, CMD_INIT)
    time.sleep(0.05)
    set_cmd(bus, CMD_START_SW)

    points = NUM_INCS + 1
    results = []

    for i in range(points):
        st = wait_valid(bus, timeout_s=5.0)
        if st is None:
            last = r8(bus, AD593x, REG_STATUS)
            raise RuntimeError(f"Timeout waiting for valid data. STATUS=0x{last:02X}")

        time.sleep(0.02)
        real, imag = read_complex(bus)
        freq = START_HZ + i * INC_HZ

        results.append({
            "freq_hz": freq,
            "real": real,
            "imag": imag,
            "status": st,
        })

        if i != points - 1:
            set_cmd(bus, CMD_INC_FREQ)

    set_cmd(bus, CMD_STANDBY)
    return results

def average_sweeps(all_sweeps):
    num_sweeps = len(all_sweeps)
    num_points = len(all_sweeps[0])

    averaged = []
    for i in range(num_points):
        freq = all_sweeps[0][i]["freq_hz"]
        real_avg = sum(s[i]["real"] for s in all_sweeps) / num_sweeps
        imag_avg = sum(s[i]["imag"] for s in all_sweeps) / num_sweeps

        mag = math.sqrt(real_avg * real_avg + imag_avg * imag_avg)
        phase = math.degrees(math.atan2(imag_avg, real_avg))

        averaged.append({
            "freq_hz": freq,
            "real": real_avg,
            "imag": imag_avg,
            "magnitude": mag,
            "phase_deg": phase,
        })
    return averaged

def measure_channel(bus, ch):
    mux_select(ch)
    time.sleep(0.5)

    # Optional flush sweep
    _ = run_single_sweep(bus)
    time.sleep(0.2)

    sweeps = []
    for _ in range(SWEEPS_PER_CHANNEL):
        sweeps.append(run_single_sweep(bus))
        time.sleep(0.05)

    return average_sweeps(sweeps)

def main():
    GPIO.setmode(GPIO.BCM)
    for p in (S0, S1, S2, S3):
        GPIO.setup(p, GPIO.OUT, initial=GPIO.LOW)

    try:
        with SMBus(BUS) as bus:
            all_results = {}

            for ch in range(16):
                print(f"\n=== Channel {ch} ===")
                results = measure_channel(bus, ch)
                all_results[ch] = results

                for point in results:
                    print(
                        f"[CH{ch:02d}] "
                        f"{point['freq_hz']:7d} Hz  "
                        f"Re={point['real']:9.2f}  "
                        f"Im={point['imag']:9.2f}  "
                        f"|X|={point['magnitude']:10.2f}  "
                        f"phase={point['phase_deg']:7.2f}°"
                    )

    finally:
        GPIO.cleanup()

if __name__ == "__main__":
    main()
