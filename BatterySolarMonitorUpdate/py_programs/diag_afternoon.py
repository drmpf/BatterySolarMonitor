"""
Diagnostic: pull today's (or most recent clear-day's) hourly actual vs predicted
solar with sun position bins.

Helps identify which (az_bin, el_bin) cells are over-predicting in the
afternoon.
"""
from __future__ import annotations
from pathlib import Path
import pandas as pd
import numpy as np
import sys

sys.path.insert(0, str(Path(__file__).parent))
from solar_shading_model import solar_position, AZ_BIN, EL_BIN, SYSTEM_KW

PROJECT = Path(__file__).parent.parent
DATA = PROJECT / "data"

# Pick days to inspect: most recent days in solar_15min_history.csv
sh = pd.read_csv(DATA / "solar_15min_history.csv", dtype={"date": str})
recent = sh.tail(7)["date"].tolist()
print(f"Most recent 7 days in 15-min history: {recent}\n")

# Calibrated forecast (shading-corrected hourly)
cal = pd.read_csv(DATA / "calibrated_solar_hourly_v2.csv")
cal["timestamp"] = pd.to_datetime(cal["timestamp"])
cal["date"] = cal["timestamp"].dt.strftime("%Y-%m-%d")
cal["hour"] = cal["timestamp"].dt.hour

# Sun position with same binning as model uses
ts_idx = pd.DatetimeIndex(cal["timestamp"])
el, az = solar_position(ts_idx)
cal["el"] = el
cal["az"] = az
cal["az_bin"] = (cal["az"] // AZ_BIN * AZ_BIN).astype(int)
cal["el_bin"] = (cal["el"] // EL_BIN * EL_BIN).astype(int)

for date_str in recent:
    row = sh[sh["date"] == date_str]
    if row.empty:
        continue
    row = row.iloc[0]

    # Hourly actual = mean of 4 slots in that hour
    hourly_actual = []
    for h in range(24):
        slots = [float(row[f"slot_{h*4+j}"]) for j in range(4)]
        hourly_actual.append(sum(slots) / 4)

    fc = cal[cal["date"] == date_str].sort_values("hour")
    if fc.empty:
        continue

    # daily totals
    fc_total = fc["est_kw_shading"].sum()
    act_total = sum(hourly_actual)
    print(f"=== {date_str}  forecast_total={fc_total:.1f} kWh  actual_total={act_total:.1f} kWh ===")
    print(f"{'hr':>3} {'az':>4} {'el':>4} {'azB':>4} {'elB':>4} {'shadeF':>7} {'fc_kW':>6} {'act_kW':>6} {'diff':>6}")
    for _, r in fc.iterrows():
        h = int(r["hour"])
        a_kw = hourly_actual[h]
        # only show daylight rows
        if r["est_kw_shading"] < 0.01 and a_kw < 0.05:
            continue
        diff = r["est_kw_shading"] - a_kw
        flag = ""
        if a_kw > 0.5 and abs(diff) > 0.5:
            flag = "  <-- OVER" if diff > 0 else "  <-- under"
        print(f"{h:>3d} {r['az']:>4.0f} {r['el']:>4.0f} "
              f"{r['az_bin']:>4d} {r['el_bin']:>4d} "
              f"{r['shade_factor']:>7.3f} "
              f"{r['est_kw_shading']:>6.2f} {a_kw:>6.2f} {diff:>+6.2f}{flag}")
    print()
