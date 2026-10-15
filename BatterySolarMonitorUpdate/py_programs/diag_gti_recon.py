"""
Reconstruct GTI from direct_radiation + diffuse_radiation using POA geometry
(isotropic sky model) and compare to OM's reported global_tilted_irradiance
on the morning bins where actual production exceeds the OM prediction.

If our reconstruction agrees with OM's GTI, the upstream direct/diffuse
values are also too low — option (c) won't help.

If reconstruction is significantly higher than OM's GTI at low sun, we
have a path to better predictions.
"""
from __future__ import annotations
from pathlib import Path
import sys
import pandas as pd
import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
from solar_shading_model import solar_position, SYSTEM_KW
from fetch_solar_history import PANEL_TILT, PANEL_AZIMUTH as PANEL_AZIMUTH_OM

DATA = Path(__file__).parent.parent / "data"

# PANEL_TILT, PANEL_AZIMUTH_OM imported above -- degrees from horizontal /
# OM convention: 0=south, -90=east, +90=west
# Convert OM azimuth to "0=N clockwise" convention used by solar_position():
#   OM 0 (S) -> 180; OM -90 (E) -> 90; OM +90 (W) -> 270; OM -180 (N) -> 0
# Formula: panel_az_NCW = (180 + panel_az_OM) mod 360
PANEL_AZIMUTH_NCW = (180.0 + PANEL_AZIMUTH_OM) % 360.0
print(f"Panel: tilt={PANEL_TILT}deg, azimuth={PANEL_AZIMUTH_OM}deg (OM) "
      f"= {PANEL_AZIMUTH_NCW}deg (0=N clockwise)")

ALBEDO = 0.20  # ground reflectance for ground-reflected component

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

# OM hourly with sun position
om = pd.read_csv(DATA / "solar_history.csv")
om["timestamp"] = pd.to_datetime(om["timestamp"])
om["gti_om"] = om["global_tilted_irradiance"].fillna(0)
om["dir_h"]  = om["direct_radiation"].fillna(0)    # horizontal direct
om["dif_h"]  = om["diffuse_radiation"].fillna(0)   # horizontal diffuse
ts_idx = pd.DatetimeIndex(om["timestamp"])
el, az = solar_position(ts_idx)
om["el"] = el
om["az"] = az
om["date_str"] = om["timestamp"].dt.strftime("%Y-%m-%d")
om["hour"] = om["timestamp"].dt.hour

# DNI = direct_horizontal / sin(elevation)  (clipped to avoid div by 0 at horizon)
sin_el = np.sin(np.radians(np.maximum(om["el"].values, 0.5)))
om["dni"] = np.where(om["el"] > 0.5, om["dir_h"].values / sin_el, 0.0)

# AOI on panel: cos(AOI) = sin(el)*cos(tilt) + cos(el)*sin(tilt)*cos(sun_az - panel_az)
tilt_rad = np.radians(PANEL_TILT)
panel_az_rad = np.radians(PANEL_AZIMUTH_NCW)
el_rad = np.radians(om["el"].values)
az_rad = np.radians(om["az"].values)

cos_aoi = (np.sin(el_rad) * np.cos(tilt_rad)
           + np.cos(el_rad) * np.sin(tilt_rad)
             * np.cos(az_rad - panel_az_rad))
cos_aoi = np.clip(cos_aoi, 0.0, 1.0)   # negative AOI = back of panel

# POA components (isotropic sky model)
poa_direct  = om["dni"].values * cos_aoi
poa_diffuse = om["dif_h"].values * (1 + np.cos(tilt_rad)) / 2
ghi         = om["dir_h"].values + om["dif_h"].values
poa_refl    = ghi * ALBEDO * (1 - np.cos(tilt_rad)) / 2
om["gti_recon"] = poa_direct + poa_diffuse + poa_refl

# Also compute DNI-only contribution as a fraction
om["poa_direct"]  = poa_direct
om["poa_diffuse"] = poa_diffuse
om["poa_refl"]    = poa_refl

joined = hourly_actual.merge(
    om[["date_str", "hour", "az", "el", "gti_om", "gti_recon",
        "dir_h", "dif_h", "dni", "poa_direct", "poa_diffuse", "poa_refl"]],
    on=["date_str", "hour"], how="inner")

# Show hour 7 details across all 45 days
print(f"\nHour 7 details — OM GTI vs reconstructed GTI vs actual production:")
print(f"{'date':<12} {'el':>4} {'dir_h':>5} {'dif_h':>5} {'dni':>5} "
      f"{'POAdir':>6} {'POAdif':>6} {'GTI_om':>6} {'GTI_rec':>7} "
      f"{'kW_om':>5} {'kW_rec':>6} {'actual':>6}")
sub = joined[joined["hour"] == 7].sort_values("date_str")
for _, r in sub.iterrows():
    kw_om = SYSTEM_KW * r["gti_om"] / 1000.0
    kw_rec = SYSTEM_KW * r["gti_recon"] / 1000.0
    print(f"{r['date_str']:<12} {r['el']:>4.0f} {r['dir_h']:>5.0f} {r['dif_h']:>5.0f} "
          f"{r['dni']:>5.0f} {r['poa_direct']:>6.0f} {r['poa_diffuse']:>6.0f} "
          f"{r['gti_om']:>6.0f} {r['gti_recon']:>7.0f} "
          f"{kw_om:>5.2f} {kw_rec:>6.2f} {r['actual_kw']:>6.2f}")

# Aggregate: mean ratio actual / kw_recon per hour
print(f"\nMean implied SF per morning hour (recon vs OM):")
print(f"{'hr':>3}  {'n':>4}  {'mean_act':>8}  {'mean_kw_om':>10}  "
      f"{'mean_kw_rec':>11}  {'sf_om':>5}  {'sf_rec':>6}")
for h in range(6, 12):
    grp = joined[joined["hour"] == h]
    if len(grp) < 3:
        continue
    kw_om = SYSTEM_KW * grp["gti_om"].mean() / 1000.0
    kw_rec = SYSTEM_KW * grp["gti_recon"].mean() / 1000.0
    a = grp["actual_kw"].mean()
    sf_om = a / kw_om if kw_om > 0.01 else float('nan')
    sf_rec = a / kw_rec if kw_rec > 0.01 else float('nan')
    print(f"{h:>3}  {len(grp):>4d}  {a:>8.2f}  {kw_om:>10.2f}  "
          f"{kw_rec:>11.2f}  {sf_om:>5.2f}  {sf_rec:>6.2f}")
