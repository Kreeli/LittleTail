#!/usr/bin/env python3
"""
Cross-check the audio clock configuration against the sources.

Two independent things are verified:

A) I2S2 -> ES9018 sample rate accuracy
   The PLL3 settings are read straight out of User/Syscfg.c and the divider plus
   the achieved sample rate are re-derived with exactly the same integer
   arithmetic the C code uses, so a change in one place cannot silently drift
   away from what this project claims.

       fI2SCLK = HSE / PREDIV2 * PLL3MUL       (HSE = 8 MHz on this board)
       Fs      = fI2SCLK / (64 * (2*I2SDIV + ODD))
       D = round(fI2SCLK / (64 * Fs)), clamped to [4, 511]
       I2SPR = I2SDIV | (ODD << 8)

B) ES9018K2M clock constraints (Datasheet v3.7)
   * BCLK = 64 x FRAME_clk for 32-bit data   (p.10, format table note)
   * serial PCM normal mode needs MCLK > 192 x FSR   (p.7)
   * chip master mode: BCLK = MCLK/4, /8, /16 and FRAME = BCLK/64  (Reg 0x0A)
   ES9018_MCLK_HZ / ES9018_MCLK_MIN_RATIO are read out of User/ES9018.h.

Exit code 0 when every check passes, 1 otherwise.

Usage:  python check_i2s_source.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SYSCFG = os.path.join(ROOT, "User", "Syscfg.c")
ES9018_H = os.path.join(ROOT, "User", "ES9018.h")
RATES = [12000, 24000, 48000, 96000]
HSE = 8_000_000
TOL = 0.5          # percent, sample-rate accuracy
BCLK_PER_FRAME = 64   # 32 bit x 2 channels


def mul_value(tag):
    """'10' -> 10, '12_5' -> 12.5, '2_5' -> 2.5"""
    return float(tag.replace("_", "."))


def read(path):
    try:
        return open(path, encoding="utf-8").read()
    except OSError as exc:
        print("cannot read %s: %s" % (path, exc))
        sys.exit(1)


def check_i2s_rates():
    text = read(SYSCFG)
    m = re.search(r"#define\s+I2S2_PLL3_HZ\s+(\d+)u?", text)
    p = re.search(r"RCC_PREDIV2_Div(\d+)", text)
    q = re.search(r"RCC_PLL3Mul_([0-9_]+)", text)
    if not (m and p and q):
        print("could not find I2S2_PLL3_HZ / RCC_PREDIV2_Div / RCC_PLL3Mul in Syscfg.c")
        return 1

    claimed = int(m.group(1))
    prediv2 = int(p.group(1))
    mul = mul_value(q.group(1))
    vco = HSE / prediv2 * mul

    print("A) I2S2 sample rate  (User/Syscfg.c)")
    print("   PREDIV2 = %d, PLL3MUL = %g  ->  PLL3 VCO = %g Hz" % (prediv2, mul, vco))
    rc = 0
    if abs(vco - claimed) > 1.0:
        print("   FAIL: PLL3 settings do not produce I2S2_PLL3_HZ (%d)" % claimed)
        rc = 1
    else:
        print("   OK: PLL3 settings match #define I2S2_PLL3_HZ %d" % claimed)

    fi2s = int(round(vco))
    print()
    print("   rate      I2SDIV  ODD  D    achieved Hz   error")
    worst = 0.0
    for fs in RATES:
        d = (fi2s + 32 * fs) // (64 * fs)       # same expression as the C code
        d = min(max(d, 4), 511)
        odd = d & 1
        i2sdiv = (d - odd) // 2
        f_act = fi2s / (64.0 * d)
        err = (f_act - fs) / fs * 100.0
        worst = max(worst, abs(err))
        print("   %6d    %4d  %3d  %3d   %10.1f   %+7.4f%%" %
              (fs, i2sdiv, odd, d, f_act, err))
    print("   worst |error| = %.4f%%  (%.2f cents)" % (worst, worst / 100.0 * 1731.0))
    if worst > TOL:
        print("   FAIL: more than %.2f%% off -> audible pitch error" % TOL)
        rc = 1
    else:
        print("   OK: all four rates within %.2f%%" % TOL)

    # the feedback value handed to the host: 16.16 samples per microframe.
    # This is what makes the host send data at OUR consumption rate instead of
    # the nominal one, which is what stops the buffer from drifting.
    print()
    print("   feedback to host = 16.16 samples/microframe (fb = fI2S*65536/(64*D*8000)):")
    for fs in RATES:
        d = (fi2s + 32 * fs) // (64 * fs)
        d = min(max(d, 4), 511)
        fb = (fi2s << 16) // (64 * d * 8000)
        nominal = fs * 65536 // 8000
        print("   %6d Hz -> fb = %8d (0x%08X)  nominal= %8d  diff= %+5.2f%%" %
              (fs, fb, fb, nominal, (fb - nominal) / nominal * 100.0))
    return rc


def check_es9018_clock():
    text = read(ES9018_H)
    m = re.search(r"#define\s+ES9018_MCLK_HZ\s+(\d+)u?", text)
    r = re.search(r"#define\s+ES9018_MCLK_MIN_RATIO\s+(\d+)u?", text)
    if not (m and r):
        print("could not find ES9018_MCLK_HZ / ES9018_MCLK_MIN_RATIO in ES9018.h")
        return 1

    mclk = int(m.group(1))
    ratio = int(r.group(1))
    rc = 0

    print()
    print("B) ES9018K2M clock constraints  (User/ES9018.h)")
    print("   MCLK (XI crystal) = %d Hz, requirement MCLK > %d x FSR" % (mclk, ratio))
    print()
    print("   Fs       BCLK=64*Fs   MCLK/FRAME   MCLK/BCLK   MCLK>%dxFSR" % ratio)
    for fs in RATES:
        bclk = BCLK_PER_FRAME * fs
        ok = mclk > ratio * fs
        if not ok:
            rc = 1
        print("   %6d   %8.3f MHz   %9.0f   %8.1f   %s" %
              (fs, bclk / 1e6, mclk / fs, mclk / bclk, "OK" if ok else "FAIL"))

    max_fsr = mclk / ratio
    print()
    print("   highest FSR allowed by MCLK > %d x FSR: %.1f kHz" % (ratio, max_fsr / 1000.0))
    print("     192 kHz would need the synchronous MCLK = 128 x FSR mode")
    print("     (for 24.576 MHz that is exactly 192 kHz)")

    # This is the part that was wrong: Reg 0x0A clock_divider_select must track Fs.
    print()
    print("   Reg 0x0A clock_divider_select  (MCLK/BCLK must be 4, 8 or 16):")
    print("   Fs       BCLK=64*Fs   needed ratio   divider   code")
    for fs in RATES:
        need = mclk / (BCLK_PER_FRAME * fs)
        if need in (4, 8, 16):
            print("   %6d   %8.3f MHz   %9.0f   MCLK/%-3d  2'b%s" %
                  (fs, BCLK_PER_FRAME * fs / 1e6, need, int(need),
                   {4: "00", 8: "01", 16: "10"}[int(need)]))
        else:
            print("   %6d   %8.3f MHz   %9.0f   NOT AVAILABLE (chip has 4/8/16 only)"
                  % (fs, BCLK_PER_FRAME * fs / 1e6, need))
    print("   -> 12 kHz needs ratio 32 which does not exist, so it must NOT be")
    print("      advertised in the UAC2 rate table (it would play 2x too fast).")

    # cross-check the advertised rate list in usb_app.c against what is usable
    app = read(os.path.join(ROOT, "User", "usb_app.c"))
    listed = [int(v) for v in re.findall(r"AUDIO_RANGE_DISCRETE\((\d+)\)", app)]
    print()
    print("   advertised rates in usb_app.c: %s" % listed)
    for fs in listed:
        need = mclk / (BCLK_PER_FRAME * fs)
        ok = (need in (4, 8, 16)) and (mclk > ratio * fs)
        if not ok:
            print("   FAIL: %d Hz is advertised but its MCLK/BCLK ratio is %.0f" % (fs, need))
            rc = 1
    return rc


def main():
    rc = check_i2s_rates()
    rc |= check_es9018_clock()
    print()
    print("RESULT: %s" % ("PASS" if rc == 0 else "FAIL"))
    return rc


if __name__ == "__main__":
    sys.exit(main())
