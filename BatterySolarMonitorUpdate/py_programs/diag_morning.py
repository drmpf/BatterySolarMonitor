"""
Diagnose morning under-prediction by computing implied shade factors
(actual_kw / base_kw, where base_kw = SYSTEM_KW * gti / 1000) for the
morning bins.  If implied SF > 1.3, the NNLS solver is bound-capped
and cannot fit — pointing to GTI underestimation rather than shading.
"""
from __future__ import annotations
from pathlib import Path
import sys
import pandas as pd
import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
from solar_shading_model import solar_position, AZ_BIN, EL_BIN, SYSTEM_KW

DATA = Path(__file__).parent.parent / "data"

# Hourly actual from 15-min slots
sh = pd.read_csv(DATA / "solar_15min_history.csv", dtype={"date": str})
hourly_rows = []
for _, row in sh.iterrows():
    d = row["date"]
    for h in range(24):
        avg_kw = sum(row[f"slot_{h*4+j}"] for j in range(4)) / 4
        if avg_kw > 0.01:
            hourly_rows.append({"date_str": d, "hour": h, "actual_kw": avg_kw})
hourly_actual = pd.DataFrame(hourly_rows)

# OM hourly with sun position + GTI
om = pd.read_csv(DATA / "solar_history.csv")
om["timestamp"] = pd.to_datetime(om["timestamp"])
om["gti"] = om["global_tilted_irradiance"].fillna(0)
om["base_kw"] = SYSTEM_KW * om["gti"] / 1000.0
ts_idx = pd.DatetimeIndex(om["timestamp"])
el, az = solar_position(ts_idx)
om["el"] = el
om["az"] = az
om["az_bin"] = (om["az"] // AZ_BIN * AZ_BIN).astype(int)
om["el_bin"] = (om["el"] // EL_BIN * EL_BIN).astype(int)
om["date_str"] = om["timestamp"].dt.strftime("%Y-%m-%d")
om["hour"] = om["timestamp"].dt.hour

joined = hourly_actual.merge(om[["date_str", "hour", "az", "el", "az_bin", "el_bin", "gti", "base_kw"]],
                              on=["date_str", "hour"], how="inner")
joined = joined[joined["base_kw"] > 0.05].copy()
joined["sf_implied"] = joined["actual_kw"] / joined["base_kw"]

# Per-bin stats for morning bins (hours 6-11)
print("Morning bins (hours 6-11), all-day implied shade factor:")
print(f"{'hr':>3} {'az':>4} {'el':>4}  {'n_obs':>5} {'n_dates':>7}  "
      f"{'mean_sf':>7} {'p25':>5} {'p75':>5} {'min':>5} {'max':>5}  "
      f"{'mean_act':>8} {'mean_base':>9}")
print("-" * 100)
morning = joined[joined["hour"].between(6, 11)]
for (azb, elb), grp in morning.groupby(["az_bin", "el_bin"]):
    if len(grp) < 3:
        continue
    n_obs = len(grp)
    n_dates = grp["date_str"].nunique()
    mean_sf = grp["sf_implied"].mean()
    p25 = grp["sf_implied"].quantile(0.25)
    p75 = grp["sf_implied"].quantile(0.75)
    min_sf = grp["sf_implied"].min()
    max_sf = grp["sf_implied"].max()
    mean_act = grp["actual_kw"].mean()
    mean_base = grp["base_kw"].mean()
    # Most common hour for this bin
    hr = int(grp["hour"].mode().iloc[0])
    flag = "  <-- capped (>1.3)" if mean_sf > 1.3 else ""
    print(f"{hr:>3} {azb:>4} {elb:>4}  {n_obs:>5} {n_dates:>7}  "
          f"{mean_sf:>7.3f} {p25:>5.2f} {p75:>5.2f} {min_sf:>5.2f} {max_sf:>5.2f}  "
          f"{mean_act:>8.2f} {mean_base:>9.2f}{flag}")

# Specifically look at hour 7 across all days (where the worst under-prediction is)
print("\nHour 7 detail across all 45 days:")
print(f"{'date':<12} {'az':>4} {'el':>4} {'azB':>4} {'elB':>4} {'gti':>5} "
      f"{'base':>5} {'actual':>6} {'sf_impl':>7}")
for _, r in joined[joined["hour"] == 7].sort_values("date_str").iterrows():
    print(f"{r['date_str']:<12} {r['az']:>4.0f} {r['el']:>4.0f} "
          f"{r['az_bin']:>4d} {r['el_bin']:>4d} {r['gti']:>5.0f} "
          f"{r['base_kw']:>5.2f} {r['actual_kw']:>6.2f} {r['sf_implied']:>7.2f}")
