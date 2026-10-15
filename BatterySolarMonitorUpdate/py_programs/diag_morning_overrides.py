"""
Compare current model shade factor (from calibrated CSV) to empirical mean
(from solar_15min_history.csv / OM GTI) for morning bins (sun in E/NE).
Suggests override values for bins where the gap is large and data is solid.
"""
from __future__ import annotations
from pathlib import Path
import sys
import pandas as pd

sys.path.insert(0, str(Path(__file__).parent))
from solar_shading_model import solar_position, AZ_BIN, EL_BIN, SYSTEM_KW

DATA = Path(__file__).parent.parent / "data"

# Empirical SF from observed actuals
sh = pd.read_csv(DATA / "solar_15min_history.csv", dtype={"date": str})
hourly_rows = []
for _, row in sh.iterrows():
    d = row["date"]
    for h in range(24):
        avg_kw = sum(row[f"slot_{h*4+j}"] for j in range(4)) / 4
        if avg_kw > 0.01:
            hourly_rows.append({"date_str": d, "hour": h, "actual_kw": avg_kw})
hourly_actual = pd.DataFrame(hourly_rows)

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

joined = hourly_actual.merge(
    om[["date_str", "hour", "az_bin", "el_bin", "base_kw"]],
    on=["date_str", "hour"], how="inner")
# Need a base_kw threshold so noise at low GTI doesn't dominate the ratio
joined = joined[joined["base_kw"] > 0.5].copy()
joined["sf_emp"] = joined["actual_kw"] / joined["base_kw"]

# Current model SF from calibrated CSV (mean per bin)
cal = pd.read_csv(DATA / "calibrated_solar_hourly_v2.csv")
cal = cal[cal["gti_wm2"] > 10].copy()
cal["az_bin"] = (cal["sun_azimuth"] // AZ_BIN * AZ_BIN).astype(int)
cal["el_bin"] = (cal["sun_elevation"] // EL_BIN * EL_BIN).astype(int)
model_sf = cal.groupby(["az_bin", "el_bin"])["shade_factor"].mean().to_dict()

# Compare for morning quadrant (az < 100, hours 9+ — handled by floor for 7-8)
print(f"{'az':>4} {'el':>4}  {'n_obs':>5} {'n_dates':>7}  "
      f"{'emp_sf':>6} {'p25':>5} {'p75':>5}  {'model_sf':>8} "
      f"{'gap':>6} {'suggest':>7}")
print("-" * 80)
suggested = {}
for (azb, elb), grp in joined.groupby(["az_bin", "el_bin"]):
    # Morning quadrant: sun in east, NE, north — i.e. az_bin in [0..90] OR az_bin in [350]
    if not (azb < 100 or azb >= 350):
        continue
    if len(grp) < 10:
        continue
    n_obs = len(grp)
    n_dates = grp["date_str"].nunique()
    emp = grp["sf_emp"].mean()
    p25 = grp["sf_emp"].quantile(0.25)
    p75 = grp["sf_emp"].quantile(0.75)
    mdl = model_sf.get((azb, elb), float("nan"))
    gap = emp - mdl if not pd.isna(mdl) else float("nan")
    # Suggest only if gap is meaningful and emp is well-constrained (p25-p75 not too wide)
    flag = ""
    if abs(gap) >= 0.08 and (p75 - p25) <= 0.5:
        suggested[(azb, elb)] = round(emp, 2)
        flag = "  *"
    print(f"{azb:>4} {elb:>4}  {n_obs:>5} {n_dates:>7}  "
          f"{emp:>6.2f} {p25:>5.2f} {p75:>5.2f}  {mdl:>8.3f} "
          f"{gap:>+6.2f} {emp:>7.2f}{flag}")

print(f"\nSuggested overrides (gap >= 0.08, IQR <= 0.5):")
for (a, e), v in sorted(suggested.items()):
    print(f"    ({a:3d}, {e:2d}): {v},")
