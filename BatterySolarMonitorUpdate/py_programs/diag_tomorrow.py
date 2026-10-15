"""
Print tomorrow's hourly forecast with each input + floor contribution so we
can sanity-check the chart.
"""
from __future__ import annotations
from pathlib import Path
import sys
import json
import urllib.request
import urllib.parse
from datetime import date, timedelta
import pandas as pd
import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
from solar_shading_model import solar_position, AZ_BIN, EL_BIN, SYSTEM_KW, LAT, LON
from fetch_solar_history import PANEL_TILT, PANEL_AZIMUTH, POSIX_TIMEZONE, utc_naive_to_local

DATA = Path(__file__).parent.parent / "data"

# Fetch OM forecast (matches plot_today_solar.fetch_today_forecast).
# timezone=GMT+0 + utc_naive_to_local(), not an IANA name -- see
# fetch_solar_history.py's fetch_chunk_utc()/utc_naive_to_local() docstrings
# (2026-09-28): Open-Meteo's own IANA-name handling applies a single fixed,
# non-DST-aware UTC offset for the whole request, which would have been
# wrong here too whenever this 2-day forecast window is inside daylight
# saving (open_meteo_timezone itself was removed from site_settings.csv
# the same day, so this is no longer even an available option).
params = {
    "latitude": LAT,
    "longitude": LON,
    "hourly": "global_tilted_irradiance,cloud_cover",
    "tilt": PANEL_TILT, "azimuth": PANEL_AZIMUTH,
    "timezone": "GMT+0",
    "forecast_days": 2,
}
url = "https://api.open-meteo.com/v1/forecast?" + urllib.parse.urlencode(params)
with urllib.request.urlopen(url, timeout=15) as r:
    data = json.loads(r.read())
fc = pd.DataFrame({
    "timestamp": utc_naive_to_local(pd.Series(data["hourly"]["time"]), POSIX_TIMEZONE),
    "gti": [g or 0 for g in data["hourly"]["global_tilted_irradiance"]],
    "cloud_cover": [c if c is not None else 50 for c in data["hourly"]["cloud_cover"]],
})

# Sun position
ts_idx = pd.DatetimeIndex(fc["timestamp"])
el, az = solar_position(ts_idx)
fc["el"] = el
fc["az"] = az
fc["az_bin"] = (az // AZ_BIN * AZ_BIN).astype(int)
fc["el_bin"] = (el // EL_BIN * EL_BIN).astype(int)
fc["pos_bin"] = fc["az_bin"] * 100 + fc["el_bin"]

# Shade lookup from calibrated CSV
cal = pd.read_csv(DATA / "calibrated_solar_hourly_v2.csv")
cal = cal[cal["gti_wm2"] > 10].copy()
cal["az_bin"] = (cal["sun_azimuth"] // AZ_BIN * AZ_BIN).astype(int)
cal["el_bin"] = (cal["sun_elevation"] // EL_BIN * EL_BIN).astype(int)
cal["pos_bin"] = cal["az_bin"] * 100 + cal["el_bin"]
shade_lookup = cal.groupby("pos_bin")["shade_factor"].mean().to_dict()

fc["shade"] = fc["pos_bin"].map(shade_lookup).fillna(0.85)
fc["base_kw"] = SYSTEM_KW * fc["gti"] / 1000.0
fc["est_kw_raw"] = fc["base_kw"] * fc["shade"]

# Morning floor
floor_df = pd.read_csv(DATA / "morning_floor.csv")
floor_map = floor_df.set_index(["month", "hour"])["floor_kw_clear"].to_dict()
fc["month"] = fc["timestamp"].dt.month
fc["hour"]  = fc["timestamp"].dt.hour
keys = list(zip(fc["month"].astype(int), fc["hour"].astype(int)))
fc["floor_clear"] = [floor_map.get(k, 0.0) for k in keys]
cloud_factor = (1 - fc["cloud_cover"].clip(0, 100) / 100 * 0.6).clip(0.4, 1.0)
fc["floor_eff"] = fc["floor_clear"] * cloud_factor
sun_up = fc["el"] > 0
fc.loc[sun_up, "est_kw"] = fc.loc[sun_up, ["est_kw_raw", "floor_eff"]].max(axis=1)
fc.loc[~sun_up, "est_kw"] = 0.0
fc["est_kw"] = fc["est_kw"].clip(0, SYSTEM_KW)

tomorrow = date.today() + timedelta(days=1)
sub = fc[fc["timestamp"].dt.date == tomorrow]
print(f"Tomorrow {tomorrow} hourly forecast:")
print(f"{'hr':>3} {'az':>4} {'el':>4} {'gti':>4} {'cld':>4} "
      f"{'shade':>5} {'base':>5} {'est_raw':>7} {'flrCl':>5} {'flrEff':>6} "
      f"{'est':>5} {'src':<8}")
print("-" * 80)
total = 0.0
for _, r in sub.iterrows():
    h = int(r["hour"])
    if r["est_kw"] < 0.005 and r["base_kw"] < 0.005:
        continue
    src = "floor" if r.get("est_kw_raw", 0) < r.get("floor_eff", 0) and r["el"] > 0 else "model"
    print(f"{h:>3d} {r['az']:>4.0f} {r['el']:>4.0f} {r['gti']:>4.0f} "
          f"{r['cloud_cover']:>4.0f} {r['shade']:>5.2f} {r['base_kw']:>5.2f} "
          f"{r['est_kw_raw']:>7.2f} {r['floor_clear']:>5.2f} {r['floor_eff']:>6.2f} "
          f"{r['est_kw']:>5.2f} {src:<8}")
    total += r["est_kw"]
print(f"\nTomorrow total: {total:.1f} kWh")
