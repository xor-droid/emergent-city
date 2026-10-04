#!/usr/bin/env python3
"""Controlled displacement harness for the --police-bias policy knob.

Comparing two policies in a stochastic sim by eyeballing one run each is noisy: a
different policy changes the catch chance, which changes the RNG trajectory, so the
two runs diverge for reasons unrelated to the policy. This harness uses **common
random numbers**: it runs each bias mode on the *same* seeds (a paired design) and
averages the per-seed difference across many seeds, so trajectory noise cancels and
the systematic policy effect — does `money` bias displace crime from rich to poor
blocks? — stands out with an error bar.

All runs fix --neighborhoods --crime-wealth (so wealth/affluence exist) and force the
LLM off (OPENROUTER_API_KEY="") for determinism. It reads the per-victim-tier crime
counts (cr_poor/cr_mid/cr_rich) the sim already exports.

Usage:
  tools/displacement_harness.py [--seeds N] [--days D] [--bin PATH] [--modes crime,money,balanced]
"""
import argparse, csv, os, statistics, subprocess, sys, tempfile
from pathlib import Path

def run(binpath, seed, days, mode, outdir):
    out = os.path.join(outdir, f"s{seed}_{mode}.csv")
    env = dict(os.environ)
    env.update({
        "CSIM_NEIGHBORHOODS": "1",
        "CSIM_CRIME_WEALTH": "1",
        "CSIM_POLICE_BIAS": mode,
        "OPENROUTER_API_KEY": "",   # force LLM off (dotenv won't override a set var) -> deterministic
    })
    subprocess.run([binpath, "--seed", str(seed), "--days", str(days), "--metrics", out],
                   env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    row = None
    with open(out) as f:
        for row in csv.DictReader(f):
            pass            # keep the last (cumulative) row
    if not row:
        raise RuntimeError(f"no metrics rows for seed {seed} mode {mode}")
    p, m, r = int(row["cr_poor"]), int(row["cr_mid"]), int(row["cr_rich"])
    tot = p + m + r
    return {"poor": p, "mid": m, "rich": r, "total": tot,
            "poor_share": 100.0 * p / tot if tot else 0.0,
            "rich_share": 100.0 * r / tot if tot else 0.0}

def summarize(xs):
    n = len(xs); mean = statistics.fmean(xs)
    sd = statistics.stdev(xs) if n > 1 else 0.0
    return mean, sd, (sd / (n ** 0.5) if n else 0.0)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seeds", type=int, default=20, help="number of seeds (1..N)")
    ap.add_argument("--days", type=int, default=30)
    ap.add_argument("--modes", default="crime,money,balanced")
    here = Path(__file__).resolve().parent
    ap.add_argument("--bin", default=str(here.parent / "csim" / "build" / "csim_headless"))
    args = ap.parse_args()

    modes = [m.strip() for m in args.modes.split(",") if m.strip()]
    if not os.path.exists(args.bin):
        sys.exit(f"binary not found: {args.bin}  (build it: cmake --build csim/build -j1)")
    seeds = list(range(1, args.seeds + 1))

    print(f"police-bias displacement harness — {len(seeds)} seeds x {args.days} days x {modes}")
    print(f"  common random numbers (paired by seed), LLM off, --neighborhoods --crime-wealth\n")

    # per-mode: list of poor_share / rich_share across seeds (paired by index)
    data = {mode: {"poor": [], "rich": []} for mode in modes}
    with tempfile.TemporaryDirectory() as outdir:
        for s in seeds:
            for mode in modes:
                res = run(args.bin, s, args.days, mode, outdir)
                data[mode]["poor"].append(res["poor_share"])
                data[mode]["rich"].append(res["rich_share"])
            print(f"\r  ran seed {s}/{len(seeds)}", end="", flush=True)
    print("\n")

    # absolute per-mode shares
    print(f"  {'mode':<10}{'poor-target %':>16}{'rich-target %':>16}")
    for mode in modes:
        pm, _, pse = summarize(data[mode]["poor"])
        rm, _, rse = summarize(data[mode]["rich"])
        print(f"  {mode:<10}{pm:>10.1f} ±{pse:>3.1f}{rm:>10.1f} ±{rse:>3.1f}")

    # paired contrasts vs the default 'crime' policy (the clean displacement signal)
    if "crime" in modes:
        base = data["crime"]
        print(f"\n  paired vs 'crime' (per-seed diff, so trajectory noise cancels):")
        for mode in modes:
            if mode == "crime":
                continue
            dpoor = [a - b for a, b in zip(data[mode]["poor"], base["poor"])]
            drich = [a - b for a, b in zip(data[mode]["rich"], base["rich"])]
            pm, _, pse = summarize(dpoor); rm, _, rse = summarize(drich)
            sig = "significant" if abs(pm) > 2 * pse and pse > 0 else "not significant"
            arrow = "more" if pm > 0 else "less"
            print(f"    {mode:>9}: poor-target share {pm:+.1f} pp ±{pse:.1f}  (crime shifts {arrow} to poor blocks; {sig})")
            print(f"    {'':>9}  rich-target share {rm:+.1f} pp ±{rse:.1f}")

if __name__ == "__main__":
    main()
