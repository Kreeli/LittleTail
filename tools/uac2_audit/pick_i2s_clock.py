#!/usr/bin/env python3
"""
Pick the best I2S2 clock source + divider for the four supported sample rates.

Facts taken from the project sources:

  * Peripheral/inc/ch32v30x_rcc.h
        RCC_I2S2CLKSource_SYSCLK    = 0
        RCC_I2S2CLKSource_PLL3_VCO  = 1
        RCC_PREDIV2_Div1..16
        RCC_PLL3Mul in {2.5, 4, 5, ..., 16, 20}
    so the PLL3 clock is  (HSE / PREDIV2) * PLL3MUL

  * User/system_ch32v30x.c
        #define SYSCLK_FREQ_144MHz_HSE 144000000   -> HSE = 8 MHz
        APB1 = HCLK / 2                       -> PCLK1 = 72 MHz

  * Peripheral/src/ch32v30x_spi.c  I2S_Init()
        BCLK = fI2SCLK / (2*I2SDIV + ODD)
        Fs   = BCLK / (32 * packetlength),  packetlength = 2 for I2S_DataFormat_32b
        -> Fs = fI2SCLK / (64 * (2*I2SDIV + ODD))
        the library rounds the divisor with  tmp = f/64*10/Fs + 5 ; tmp/10
        and clamps I2SDIV to [2,255]  ->  (2*I2SDIV+ODD) in [4, 511]

  * IMPORTANT: I2S_Init() derives `sourceclock` from RCC_Clocks.SYSCLK_Frequency,
    so it is only correct when the I2S2 clock source really is SYSCLK.
    If PLL3_VCO is selected, I2SPR must be written by hand.

Usage:  python pick_i2s_clock.py
"""
import sys

HSE = 8_000_000
SYSCLK = 144_000_000
PCLK1 = 72_000_000
RATES = [12000, 24000, 48000, 96000]
PACKETLEN = 2          # I2S_DataFormat_32b  -> 32 bit x 2 channels = 64 BCLK/frame
MULS = [2.5, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 20]


def divider_for(fi2s, fs):
    """Reproduce I2S_Init()'s rounding, then the hardware divider."""
    fi2s = int(round(fi2s))          # the C code divides a uint32 sourceclock
    # tmp = (uint16_t)(((fi2s / (32*packetlength)) * 10) / fs + 5);  tmp /= 10
    # note: the C code divides first (integer truncation) then multiplies by 10
    tmp = ((fi2s // (32 * PACKETLEN)) * 10) // fs + 5
    tmp //= 10
    odd = tmp & 1
    i2sdiv = (tmp - odd) // 2
    if i2sdiv < 2:
        i2sdiv, odd = 2, 0
    if i2sdiv > 0xFF:
        i2sdiv, odd = 2, 0
    return i2sdiv, odd


def achieved(fi2s, fs):
    i2sdiv, odd = divider_for(fi2s, fs)
    d = 2 * i2sdiv + odd
    return fi2s / (64.0 * d), d, i2sdiv, odd


def evaluate(name, fi2s):
    worst = 0.0
    rows = []
    for fs in RATES:
        f_act, d, i2sdiv, odd = achieved(fi2s, fs)
        err = (f_act - fs) / fs * 100.0
        worst = max(worst, abs(err))
        rows.append("      %6d Hz -> %10.1f Hz  (I2SDIV=%3d ODD=%d, D=%3d)  %+7.4f%%"
                    % (fs, f_act, i2sdiv, odd, d, err))
    print("  %-34s worst |err| = %.4f%%" % (name, worst))
    for r in rows:
        print(r)
    return worst


def main():
    cands = []
    cands.append(("SYSCLK (144 MHz)", SYSCLK))
    for p in range(1, 17):
        for m in MULS:
            vco = HSE / p * m
            if vco < 25e6 or vco > 200e6:
                continue          # keep the VCO in a sane range
            if abs(vco - round(vco)) > 1e-9 and (m * 2) % 1:
                pass
            cands.append(("PLL3 PREDIV2=%2d MUL=%-4s -> %.4g MHz" % (p, m, vco / 1e6), vco))

    scored = []
    for name, f in cands:
        worst = 0.0
        for fs in RATES:
            f_act, _, _, _ = achieved(f, fs)
            worst = max(worst, abs((f_act - fs) / fs * 100.0))
        scored.append((worst, name, f))
    scored.sort(key=lambda t: t[0])

    print("=== best 8 candidates (PLL3 VCO fed straight into the I2S2 divider) ===")
    for worst, name, f in scored[:8]:
        evaluate(name, f)
        print()

    print("=== current configuration (SYSCLK, what the library computes today) ===")
    evaluate("SYSCLK (144 MHz)", SYSCLK)
    print()
    print("=== PCLK1 reference (what the I2S2 kernel clock actually is when the")
    print("    I2S2 clock source is left at SYSCLK? -- see note in report) ===")
    evaluate("PCLK1 (72 MHz)", PCLK1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
