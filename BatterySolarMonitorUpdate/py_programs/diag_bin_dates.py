"""
Count distinct calibration dates per (az_bin, el_bin) for the afternoon zone.

Joins solar_15min_history.csv (actual hourly kW) with solar_history.csv
(GTI + sun position) to count how many distinct dates contributed to each
afternoon bin.  Highlights bins with very low date counts and shows the
date range covered.
"""
from __future__ import annotations
from pathlib import Path
import sys
import pandas as pd

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
print(f"hourly_actual: {len(hourly_actual)} rows from {hourly_actual['date_str'].nunique()} days "
      f"({hourly_actual['date_str'].min()} -> {hourly_actual['date_str'].max()})\n")

# OM hourly with sun position
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

# Filter daylight only
om_d = om[om["gti"] > 10].copy()

# Join hourly actual with OM bins
joined = hourly_actual.merge(om_d[["date_str", "hour", "az_bin", "el_bin", "base_kw"]],
                              on=["date_str", "hour"], how="inner")
# Implied shade factor (only meaningful when base_kw is sizable)
joined = joined[joined["base_kw"] > 0.3].copy()
joined["sf_implied"] = joined["actual_kw"] / joined["base_kw"]

# Aggregate per (az_bin, el_bin): unique dates, mean sf, range
focus_bins = [
    (280, 5), (280, 10), (280, 15),
    (290, 10), (290, 15),
    (300, 15), (300, 20), (300, 25), (300, 30),
    (310, 20), (310, 25), (310, 30),
    (320, 25), (320, 30), (320, 35), (320, 40),
    (330, 30), (330, 35), (330, 40), (330, 45),
    (340, 35), (340, 40), (340, 45),
    (350, 40), (350, 45),
]

print(f"{'az':>4} {'el':>4}  {'n_obs':>5} {'n_dates':>7}  {'mean_sf':>7} {'min_sf':>6} {'max_sf':>6}  {'date_range':<25}")
print("-" * 80)
for (azb, elb) in focus_bins:
    sub = joined[(joined["az_bin"] == azb) & (joined["el_bin"] == elb)]
    if sub.empty:
        print(f"{azb:>4} {elb:>4}  {'0':>5} {'0':>7}  {'-':>7} {'-':>6} {'-':>6}  -")
        continue
    n_obs = len(sub)
    n_dates = sub["date_str"].nunique()
    mean_sf = sub["sf_implied"].mean()
    min_sf = sub["sf_implied"].min()
    max_sf = sub["sf_implied"].max()
    drange = f"{sub['date_str'].min()} -> {sub['date_str'].max()}"
    flag = ""
    in_zone = azb in {280, 290, 300, 310} and elb < 50
    if in_zone:
        flag = "  (in tree zone)"
    print(f"{azb:>4} {elb:>4}  {n_obs:>5} {n_dates:>7}  {mean_sf:>7.3f} {min_sf:>6.3f} {max_sf:>6.3f}  {drange}{flag}")

# Also show the spread of dates per problem bin (months covered)
print("\nMonth coverage for problem bins:")
for (azb, elb) in [(320, 30), (320, 35), (330, 35), (330, 40), (310, 25), (300, 20)]:
    sub = joined[(joined["az_bin"] == azb) & (joined["el_bin"] == elb)]
    if sub.empty:
        continue
    months = sorted(set(d[:7] for d in sub["date_str"]))
    print(f"  ({azb},{elb}): months = {months}  (n_obs={len(sub)})")
