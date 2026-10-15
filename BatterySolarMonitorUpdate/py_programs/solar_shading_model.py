#!/usr/bin/env python3
"""
Back-solve for a horizon shading mask from Enphase actual vs OM predicted.

For each sun position (azimuth, elevation), compute a shade factor:
  actual_daily ≈ Σ_hours (system_kw × GTI/1000 × shade_factor[sun_pos])

The shade_factor at each sun position tells us what fraction of OM-predicted
production actually occurs. Values < 1 indicate shading; > 1 indicates
OM underestimates (reflections, albedo, model error).

Uses 1,005 days of Enphase daily totals + Open-Meteo hourly GTI to solve
a constrained linear regression for the shade factors.

Reads:
  - data/enphase_daily.csv
  - data/solar_history.csv

Produces:
  - data/calibrated_solar_hourly_v2.csv  (shading-corrected hourly estimates)
  - docs/solar_shading_stats.md
  - analysis/charts/solar_shading_model.png
"""

from __future__ import annotations

import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.colors as mcolors
import numpy as np
import pandas as pd
from scipy.optimize import lsq_linear

PROJECT_DIR = Path(__file__).parent          # py_programs/
ROOT_DIR = PROJECT_DIR.parent                # BatterySolarMonitorUpdate/
DATA_DIR = ROOT_DIR / "data"
CHART_DIR = ROOT_DIR / "docs" / "charts"

sys.path.insert(0, str(PROJECT_DIR))
from fetch_solar_history import LAT, LON  # noqa: E402 -- THE Python-side source for these (see its own module docstring)

# One level above BatterySolarMonitorUpdate/ -- this whole pipeline's own
# entirely human-maintained config file (see auth/enphase_token.py's own
# module docstring). system_kw isn't in fetch_solar_history.py, so read
# directly here rather than via another module's import.
CONFIG_FILE = ROOT_DIR.parent / "site_settings.csv"


def _load_kv_csv(path: Path) -> dict[str, str]:
    """
    Load a key,value CSV (header first) into a dict. Empty dict if missing.
    Splits each line on its FIRST comma only, and strips both the whole
    line and the split-off value, matching
    BatterySolarMonitor/SiteSettings.cpp's own loadSiteConfig() exactly --
    NOT a proper quoted-CSV parse (see auth/enphase_token.py's own copy of
    this helper for the full story: posix_timezone's own value contains
    commas, and a hand-edited "key, value" row must not gain a stray
    leading space in its value).
    """
    data: dict[str, str] = {}
    if path.exists():
        with open(path) as f:
            next(f, None)  # header row
            for line in f:
                line = line.strip()
                if not line or "," not in line:
                    continue  # blank/malformed line -- skip
                key, value = line.split(",", 1)
                data[key] = value.strip()
    return data


_config = _load_kv_csv(CONFIG_FILE)
if not _config.get("system_kw"):
    print(f"ERROR: {CONFIG_FILE} is missing a value for 'system_kw' "
          f"(see auth/site_settings.csv.sample).")
    sys.exit(1)

# System parameters
SYSTEM_KW = float(_config["system_kw"])
STD_MERIDIAN = 150.0  # AEST (UTC+10)

# Recency weighting for the NNLS fit: training rows are weighted by an
# exponential decay so recent years count more than old ones (trees have
# likely grown since the 2023 data was collected, so old rows understate
# current shading). Weight halves every RECENCY_HALF_LIFE_YEARS relative to
# the most recent training date. Set to None to disable (equal weighting).
RECENCY_HALF_LIFE_YEARS = 3.0

# Bin resolution
AZ_BIN = 10  # degrees
EL_BIN = 5   # degrees

C_TEXT = "#333333"
C_LIGHT = "#CCCCCC"

# ── Tree shadow parameters ────────────────────────────────────────────────────
# Trees are approximately 10 m from the house in the WNW direction.
# Calibrated from 17 days of actual hourly production (Mar-Apr 2026).
# Per-day implied shade factor analysis (actual_kw / base_kw) in WNW zone:
#
#   el_bin  n   mean_sf  notes
#   ------  --  -------  -----------------------------------------------
#    5       8   0.230   5pm low sun; high variance (diffuse + cloud mix)
#   10       8   0.138   deep shadow; n=8
#   15       2   0.189   limited data
#   20      14   0.244   consistent shading; n=14
#   30      14   0.355   Apr 15h (el≈33°); partial clearing at tree edge
#   35       2   0.165   Mar 16h (el≈35°); deeper shadow at this az
#   40      14   0.610   Apr 14h (el≈42°, az≈317°); high variance (max 1.34)
#   45       2   0.496   sparse; likely cloud-contaminated
#
# el_bin=40-45 shows high variance (max > 1.0 from cloud diffuse), but
# mean still well below 1.0 — tree crown extends above ~38° at az≈310-320°.
# _TREE_EL_ONSET raised to 50 to capture el_bin=40-45 correction.
#
# Profile uses mean values from clear-day analysis; el_bin=35 value uses
# its own low mean (0.165) which reflects deeper shadow at March az (≈300°)
# vs April az (≈310-315°) at el_bin=30. The non-monotonic dip at el_bin=35
# is real — it corresponds to a different azimuth sub-bin where the tree
# crown is denser.
#
# To refine as more seasonal data accumulates:
#   - _TREE_AZ_BINS: azimuth bins the tree covers (expand if evidence for wider)
#   - _TREE_EL_ONSET: elevation bin where shading starts (no correction at/above)
#   - _TREE_SF_PROFILE: (el_bin, shade_factor) calibration points
#
# Affected hours (from geometry, varies by month):
#   Jan/Dec: none   Feb/Oct: 17h only
#   Mar/Sep: 15-17h   Apr-Aug: 14-16h (core affected window)
#_TREE_AZ_BINS = {280, 290, 300, 310}   # az_bin values = 280-319° (WNW)
#_TREE_EL_ONSET = 50                    # el_bin: no tree shading at or above this

# Calibration: (el_bin, shade_factor) from per-day implied sf analysis,
# Mar-Apr 2026 actual production vs Open-Meteo GTI base_kw.
#_TREE_SF_PROFILE = [
#    (0,  0.10),   # extrapolated minimum — diffuse-only below horizon
#    (5,  0.23),   # 5pm autumn (el≈9°); n=8 mean_sf=0.230
#    (10, 0.14),   # deep shadow; n=8 mean_sf=0.138
#    (15, 0.19),   # n=2 mean_sf=0.189
#    (20, 0.24),   # n=14 mean_sf=0.244 — consistent
#    (30, 0.36),   # Apr 15h (el≈33°); n=14 mean_sf=0.355
#    (35, 0.17),   # Mar 16h (el≈35°, az≈300°); n=2 mean_sf=0.165 — deeper shadow
#    (40, 0.55),   # Apr 14h (el≈42°, az≈317°); n=14 mean_sf=0.610 (high var)
#    (45, 0.50),   # sparse; n=2 mean_sf=0.496
#    (50, 1.00),   # threshold — correction only below this bin
#]
_TREE_AZ_BINS = {280, 290, 300, 310}   # unchanged
_TREE_EL_ONSET = 50   # unchanged

_TREE_SF_PROFILE = [
    ( 0, 0.100),
    ( 5, 0.081),   # forced update 2026-09-28 despite gap=-0.149 just under the 0.15 threshold (n=104) -- not yet re-fit, pending re-run
    (10, 0.140),
    (15, 0.190),
    (20, 0.240),
    (25, 0.130),   # updated from 0.130  (n=6, gap=-0.170)
    (30, 0.360),
    (35, 0.397),   # updated 2026-09-10 from 0.170 (n=68, gap=+0.227) -- analyse_tree_shadow.py, accumulated 2025-09-01..2026-09-07 evidence
    (40, 0.550),
    (45, 0.500),
    (50, 1.000),
]

# ── East tree shadow parameters (provisional) ─────────────────────────────────
# A second tree, east of the house, is believed to shade summer mornings
# (property owner's direct observation, 2026-09-08). Unlike the WNW tree,
# there is no season of 15-min hourly actuals covering this azimuth yet:
# solar_15min_history.csv only starts 2026-03-24, and the sun's morning
# azimuth never reached az 80-140deg during that window (it topped out
# around az_bin=80 near the Mar equinox) -- see the "Easterly Shading"
# section of docs/solar_shading_model.md for the full data-availability
# writeup.
#
# Provisional calibration (2026-09-08): instead of per-bin hourly data, this
# comes from a 2-parameter regression against REAL summer daily totals in
# enphase_daily.csv (2023-2026), pooling days by distance from the Dec-21
# solstice (folds Nov together with Jan-Feb) so partial summers from 4
# different calendar years combine into one estimate -- 454 days total.
# Each day's unshaded predicted energy (base_kw = 7.6 x GTI/1000) was split
# into "east zone" (az 80-140deg) vs "rest of day", further split by
# elevation, and solved against actual production:
#   el <= 15deg:      SF = 0.157   (n=454, all years combined)
#   el 15-25deg:      SF = 0.497
#   el > 25deg:        no evidence either way -- not corrected
# This produces a plausible tree-shaped gradient (deep low, tapering
# higher), matching the qualitative shape of the WNW tree profile. BUT the
# same fit run separately per summer season ranges from SF=0.13 to SF=0.65
# across 2023-2026 -- much less stable than the WNW tree's per-bin
# calibration (IQR <= 0.2 on n>=19 in that case). This is a *conservative
# provisional* correction, not a tightly-pinned one: it exists because an
# uncorrected NNLS fit at these bins runs 1.0-1.07 (i.e. treats this zone as
# essentially unshaded), which is worse than a soft, elevation-graded
# correction given what's known about the site. Refine (or drop) once a
# real Nov-Feb season of 15-min hourly actuals is available -- see
# diag_bin_dates.py / diag_morning_overrides.py, restricted to az 80-140.
_EAST_TREE_AZ_BINS = {80, 90, 100, 110, 120, 130}
_EAST_TREE_EL_ONSET = 30   # el_bin: no correction at/above -- unmeasured above el=25, don't extrapolate

_EAST_TREE_SF_PROFILE = [
    (0,  0.157),
    (25, 0.497),
]

# Per-(az_bin, el_bin) shade factor overrides derived empirically from
# solar_15min_history.csv (mean of actual_kw / base_kw on days the sun
# visited that bin).  Applied AFTER apply_tree_shadow_correction, so these
# take precedence over both the NNLS solver and the tree profile.
#
# Calibration source: 45 days of hourly actuals, Mar-May 2026, extended with
# 167 days (Mar-Sep 2026) as of the 2026-09-08 recalibration.
# Only bins with >=6 distinct dates are listed; bins below threshold fall
# back to the prior pipeline (NNLS or tree profile).
#
# Why these bins are needed: the tree shadow's az coverage was set to
# {280-310} but in May the afternoon sun (hours 13-14) passes through
# az_bin=320 and 330, which were getting NNLS values near 1.0.  The az=330
# zone shows mild shading (tree edge), while az=320 at low el is deep
# shadow comparable to az=300.  These cannot be captured by an el-only
# profile because the implied sf depends strongly on az at the same el.
_OBSERVED_SHADE_OVERRIDES = {
    # (az_bin, el_bin): shade_factor   # n_dates  months   notes
    # ── Afternoon (obstruction zone, az 320-350) ────────────────────────
    (340, 30): 0.44,                   # n=67    May-Jul  (updated 2026-09-08, was 0.35/n~10 -- much larger sample now available)
    (320, 25): 0.31,                   # n=35    May-Jun  winter 2pm (diag_bin_dates mean=0.314)
    (320, 30): 0.24,                   # n=14    Apr-May
    (320, 35): 0.30,                   # n= 6    Apr
    (330, 35): 0.61,                   # n=19    Apr-May  (updated 2026-09-08, was 0.73/n=10 -- fresh data shows more shading)
    (330, 40): 0.75,                   # n=14    Apr
    (330, 45): 0.80,                   # n=13    Apr
    (340, 35): 0.61,                   # n=20    Jul-Aug  (added 2026-09-08; NNLS had this near 1.0, uncorrected)
    # ── Near-noon / early afternoon (northward obstruction, az=350) ─────
    (350, 30): 0.81,                   # n=46    Apr-Jul  winter near-noon (updated 2026-07-28, was 0.71/n=18)
    (350, 35): 0.89,                   # n=23    Jun
    (350, 40): 0.85,                   # n=16    Apr-May
    (350, 50): 0.75,                   # n=11    Apr
    # ── Morning / mid-morning (NNLS under-estimates by 0.10-0.40) ──────
    # Empirical mean implied SF (actual_kw / OM base_kw) on bins with
    # n_dates >= 11 and IQR <= 0.5.  Most of these are bins where the
    # NNLS solver pulled the value below 1.0 due to the prior, but the
    # data suggests 1.0+ on average.  The morning-floor (build_morning_floor)
    # still handles hours 6-8 where OM GTI is unreliable; these overrides
    # apply at hours 9-11 where GTI is reasonable but NNLS SF is low.
    ( 10, 30): 0.97,                   # n=64    Apr-Jul  NNE morning shading (updated 2026-07-28, was 0.88/n=25)
    ( 20, 45): 0.82,                   # n=11    Apr-May
    ( 30, 25): 1.20,                   # n=31    Apr-Jul  (added 2026-07-28, large gap +0.20)
    ( 30, 30): 1.05,                   # n=23    Apr-Jul  (added 2026-07-28)
    ( 30, 35): 1.02,                   # n=18    Apr-May
    ( 40, 15): 1.35,                   # n=28    Apr-Jul  (added 2026-07-28, wide IQR -- cloud-contaminated, watch)
    ( 40, 25): 1.09,                   # n=16    Apr-May  (large gap +0.25)
    ( 40, 40): 0.99,                   # n=14    Apr
    ( 50, 15): 1.25,                   # n=15    Jun      low-el ENE, OM under-predicts
    ( 50, 30): 1.03,                   # n=23    Apr-May
    ( 60, 15): 0.87,                   # n=13    Apr-May
    ( 60, 20): 1.08,                   # n=15    Apr-May  (large gap +0.41)
    ( 70,  5): 1.65,                   # n=16    Apr-May  sunrise (NNLS-capped)
    # ── East tree (summer mornings, az 80-100) ─────────────────────────
    # Added 2026-09-10 after backfilling solar_15min_history.csv back to
    # 2025-09-01 gave this azimuth range clear-sky hourly coverage for the
    # first time (previously handled only by the provisional, season-pooled
    # _EAST_TREE_SF_PROFILE geometry estimate below). Clear-sky filtered
    # (cloud_cover < 25, same threshold build_morning_floor() uses) --
    # the unfiltered (all-weather) implied SF was noisy and non-monotonic
    # in elevation at these bins; restricting to clear-sky hours revealed
    # a plausible tree-shadow gradient at az=100 (el 10->15->20->25:
    # 0.58->0.54->0.62->0.83, deeper shade low, clearing higher).
    # Only bins with >=6 distinct clear-sky dates and IQR <= 0.5 included,
    # same acceptance bar as the rest of this table. Coverage is still
    # thin outside these 7 bins (az=80 el=10/15/25/30 and az=90 el=5/10/
    # 15/20 all had <6 clear-sky dates) -- apply_east_tree_shadow_correction()
    # still covers those from the geometry profile.
    ( 80, 20): 0.70,                   # n= 9    Oct
    ( 90, 25): 0.77,                   # n=20    Nov-Dec
    ( 90, 30): 0.99,                   # n= 8    Jan
    (100,  5): 1.52,                   # n= 6    Jan-Feb  OM underestimates, not shaded
    (100, 10): 0.58,                   # n=27    Nov-Jan
    (100, 15): 0.54,                   # n= 6    Nov-Dec
    (100, 25): 0.83,                   # n=10    Dec
}

def tufte(ax: plt.Axes) -> None:  # type: ignore[type-arg]
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.spines["left"].set_color(C_LIGHT)
    ax.spines["bottom"].set_color(C_LIGHT)
    ax.tick_params(colors=C_TEXT, labelsize=8)


def solar_position(timestamps: pd.DatetimeIndex) -> tuple[np.ndarray, np.ndarray]:
    """
    Compute solar elevation and azimuth for an array of timestamps.

    Uses Spencer (1971) equations. Timestamps are assumed to be local
    time in the site's own timezone (site_settings.csv's posix_timezone
    key, applied via fetch_solar_history.py's utc_naive_to_local()),
    which may include DST.

    We convert to UTC first, then compute solar time from UTC + longitude,
    which avoids all DST confusion.

    Returns elevation (degrees) and azimuth (degrees from North, clockwise:
    0=N, 90=E, 180=S, 270=W).
    """
    lat_rad = np.radians(LAT)

    doy = timestamps.dayofyear.values.astype(float)
    hour_local = (timestamps.hour + timestamps.minute / 60.0).values
    month = timestamps.month.values

    # Determine UTC offset: AEDT (UTC+11) roughly Oct-Mar, AEST (UTC+10) Apr-Sep
    # Precise DST rules: first Sunday of October to first Sunday of April
    # Approximation by month is within a week of the actual transition
    is_aedt = (month >= 10) | (month <= 3)
    utc_offset = np.where(is_aedt, 11.0, 10.0)

    # Convert local time to UTC
    hour_utc = hour_local - utc_offset

    # Solar declination (radians)
    gamma = 2 * np.pi * (doy - 1) / 365.0
    decl = (0.006918 - 0.399912 * np.cos(gamma) + 0.070257 * np.sin(gamma)
            - 0.006758 * np.cos(2 * gamma) + 0.000907 * np.sin(2 * gamma)
            - 0.002697 * np.cos(3 * gamma) + 0.00148 * np.sin(3 * gamma))

    # Equation of time (minutes)
    eot = 229.18 * (0.000075 + 0.001868 * np.cos(gamma)
                     - 0.032077 * np.sin(gamma)
                     - 0.014615 * np.cos(2 * gamma)
                     - 0.04089 * np.sin(2 * gamma))

    # Solar time from UTC + longitude + equation of time
    # LON / 15 converts degrees to hours east of Greenwich
    solar_hour = hour_utc + LON / 15.0 + eot / 60.0

    # Hour angle (radians, 0 = solar noon, positive = afternoon)
    ha = np.radians(15.0 * (solar_hour - 12.0))

    # Solar elevation
    sin_el = (np.sin(lat_rad) * np.sin(decl)
              + np.cos(lat_rad) * np.cos(decl) * np.cos(ha))
    sin_el = np.clip(sin_el, -1, 1)
    elevation = np.degrees(np.arcsin(sin_el))

    # Solar azimuth
    cos_el = np.cos(np.radians(elevation))
    cos_el = np.clip(cos_el, 0.001, None)  # avoid division by zero

    cos_az = (np.sin(decl) - np.sin(lat_rad) * sin_el) / (np.cos(lat_rad) * cos_el)
    cos_az = np.clip(cos_az, -1, 1)
    azimuth = np.degrees(np.arccos(cos_az))

    # Afternoon: azimuth > 180 (west of north)
    azimuth = np.where(ha > 0, 360 - azimuth, azimuth)

    return elevation, azimuth


def load_and_prepare() -> tuple[pd.DataFrame, pd.DataFrame, "pd.DataFrame | None"]:
    """
    Load data and compute solar positions.

    Returns enphase daily totals, om hourly (daylight only with bins),
    and hourly_actual (date_str, hour, actual_kw) from solar_15min_history.csv
    if available, else None.
    """
    # Enphase daily (production + consumption combined)
    enphase = pd.read_csv(DATA_DIR / "enphase_daily.csv",
                          parse_dates=["date"], index_col="date")

    # Open-Meteo hourly
    om = pd.read_csv(DATA_DIR / "solar_history.csv")
    om["timestamp"] = pd.to_datetime(om["timestamp"])
    om["gti"] = om["global_tilted_irradiance"].fillna(0)
    om["date"] = om["timestamp"].dt.date

    # Compute base predicted kW (no derating — we'll solve for the shade factor
    # which subsumes derating)
    om["base_kw"] = SYSTEM_KW * om["gti"] / 1000.0

    # Solar position
    ts_index = pd.DatetimeIndex(om["timestamp"])
    elevation, azimuth = solar_position(ts_index)
    om["sun_elevation"] = elevation
    om["sun_azimuth"] = azimuth

    # Only keep daylight hours with meaningful GTI
    om = om[om["gti"] > 10].copy()

    # Bin sun positions
    om["az_bin"] = (om["sun_azimuth"] // AZ_BIN * AZ_BIN).astype(int)
    om["el_bin"] = (om["sun_elevation"] // EL_BIN * EL_BIN).astype(int)
    om["pos_bin"] = om["az_bin"] * 100 + om["el_bin"]  # unique bin ID

    # Hourly actual production from 15-min history (slot values already in kW)
    # Each hour = mean of 4 slots.  Only keep hours with meaningful production.
    hourly_actual = None
    solar_hist_path = DATA_DIR / "solar_15min_history.csv"
    if solar_hist_path.exists():
        sh = pd.read_csv(solar_hist_path, dtype={"date": str})
        rows = []
        for _, row in sh.iterrows():
            d = row["date"]
            for h in range(24):
                avg_kw = sum(row[f"slot_{h*4+j}"] for j in range(4)) / 4
                if avg_kw > 0.01:
                    rows.append({"date_str": d, "hour": h, "actual_kw": avg_kw})
        if rows:
            hourly_actual = pd.DataFrame(rows)
            print(f"  {len(hourly_actual)} hourly actual rows from solar_15min_history.csv")

    return enphase, om, hourly_actual


def _recency_weights(date_strs, ref_date: pd.Timestamp, half_life_years: float) -> np.ndarray:
    """
    Exponential recency weight per row: 1.0 at ref_date, halving every
    half_life_years further back. Used to down-weight older training rows
    in the NNLS fit (see RECENCY_HALF_LIFE_YEARS).
    """
    dates = pd.to_datetime(pd.Series(list(date_strs)))
    age_years = (ref_date - dates).dt.days / 365.25
    return np.asarray(0.5 ** (age_years / half_life_years))


def solve_shading(
    enphase: pd.DataFrame,
    om: pd.DataFrame,
    hourly_actual: "pd.DataFrame | None" = None,
) -> tuple[dict[int, float], pd.DataFrame]:
    """
    Solve for shade factors using non-negative least squares.

    Primary equations (one per day):
        actual_kwh = Σ_bins (shade_factor[bin] × sum_of_base_kw_in_bin)

    Supplementary equations (one per hour) when hourly_actual is provided:
        actual_kw  = shade_factor[bin] × base_kw
    Both share the same units so they mix naturally in the design matrix.
    Hourly rows give the solver per-bin constraints that daily totals cannot
    provide individually, especially for bins that rarely dominate a whole day.
    """
    # Get unique bins
    all_bins = sorted(om["pos_bin"].unique())
    bin_to_idx = {b: i for i, b in enumerate(all_bins)}
    n_bins = len(all_bins)

    print(f"  {n_bins} sun-position bins")

    # Build the design matrix
    # For each day, sum predicted_kw for each bin
    om["date_str"] = om["date"].astype(str)
    dates_in_both = set(enphase.index.strftime("%Y-%m-%d")) & set(om["date_str"])
    dates_sorted = sorted(dates_in_both)

    n_days = len(dates_sorted)
    print(f"  {n_days} overlapping days")

    X = np.zeros((n_days, n_bins))
    y = np.zeros(n_days)

    for i, date_str in enumerate(dates_sorted):
        day_data = om[om["date_str"] == date_str]
        for _, row in day_data.iterrows():
            j = bin_to_idx[row["pos_bin"]]
            X[i, j] += row["base_kw"]

        y[i] = enphase.loc[date_str, "production_kwh"]

    # Filter out days with near-zero production or prediction
    valid = (y > 1) & (X.sum(axis=1) > 1)
    X = X[valid]
    y = y[valid]
    row_dates = list(np.array(dates_sorted)[valid])  # tracks each row's date for recency weighting
    n_daily = len(y)   # record count before hourly rows are appended
    print(f"  {n_daily} valid days after filtering")

    # Add hourly actual rows as supplementary observations.
    # Each row constrains one bin directly: shade[bin] * base_kw = actual_kw.
    # Build a lookup of (date_str, hour) -> (pos_bin, base_kw) from om.
    if hourly_actual is not None and not hourly_actual.empty:
        om_h = om.copy()
        om_h["hour"] = om_h["timestamp"].dt.hour
        om_lookup = om_h.set_index(["date_str", "hour"])[["pos_bin", "base_kw"]]

        hourly_X_rows = []
        hourly_y_rows = []
        hourly_date_rows = []
        for _, hrow in hourly_actual.iterrows():
            key = (hrow["date_str"], int(hrow["hour"]))
            if key not in om_lookup.index:
                continue
            om_entry = om_lookup.loc[key]
            if isinstance(om_entry, pd.DataFrame):
                om_entry = om_entry.iloc[0]
            j = bin_to_idx.get(int(om_entry["pos_bin"]))
            if j is None or om_entry["base_kw"] < 0.01:
                continue
            x_row = np.zeros(n_bins)
            x_row[j] = float(om_entry["base_kw"])
            hourly_X_rows.append(x_row)
            hourly_y_rows.append(float(hrow["actual_kw"]))
            hourly_date_rows.append(hrow["date_str"])

        if hourly_X_rows:
            X = np.vstack([X, np.array(hourly_X_rows)])
            y = np.concatenate([y, np.array(hourly_y_rows)])
            row_dates.extend(hourly_date_rows)
            print(f"  Added {len(hourly_X_rows)} hourly actual rows "
                  f"({len(set(hourly_actual['date_str']))} days)")

    # Recency weighting: down-weight older rows so the fit favours recent
    # years (trees have likely grown since the 2023 data was collected, so
    # equal-weighting would understate current shading). Applied as
    # sqrt(weight) row scaling for weighted least squares; the unweighted
    # X, y are kept as-is for fit-quality reporting and energy totals below.
    if RECENCY_HALF_LIFE_YEARS:
        ref_date = pd.to_datetime(row_dates).max()
        row_weight = _recency_weights(row_dates, ref_date, RECENCY_HALF_LIFE_YEARS)
        oldest_w = float(row_weight.min())
        print(f"  Recency weighting: half-life={RECENCY_HALF_LIFE_YEARS}y, "
              f"oldest row weight={oldest_w:.2f}, newest=1.00")
    else:
        row_weight = np.ones(len(y))
    sqrt_w = np.sqrt(row_weight)
    X_w = X * sqrt_w[:, None]
    y_w = y * sqrt_w

    # Regularised bounded least squares.
    # Physical constraints: shade factor in [0, 1.3]
    #   0   = fully shaded (or panel can't see the sun there)
    #   0.85 = typical derating (inverter, temp, wiring, soiling)
    #   1.0  = OM prediction is perfect
    #   1.3  = OM underestimates (albedo/reflection boost — generous cap)
    #
    # Add Tikhonov regularisation pulling toward 0.85 prior,
    # weighted inversely by bin energy (well-constrained bins are free;
    # poorly-constrained bins shrink toward prior). Energy is computed from
    # the recency-weighted matrix so bins with only old evidence are treated
    # as poorly-constrained, same as bins with little evidence of any age.
    prior = 0.85
    col_energy = X_w.sum(axis=0)
    median_energy = float(np.median(col_energy[col_energy > 0]))

    lam = np.zeros(n_bins)
    for j in range(n_bins):
        if col_energy[j] > 0:
            lam[j] = median_energy / max(col_energy[j], 1) * 1.5
        else:
            lam[j] = 10.0

    X_aug = np.vstack([X_w, np.diag(lam)])
    y_aug = np.concatenate([y_w, lam * prior])

    lb = np.zeros(n_bins)
    ub = np.full(n_bins, 1.3)

    result = lsq_linear(X_aug, y_aug, bounds=(lb, ub), method="bvls")
    shade_factors = result.x

    # Build result dict
    shade_dict: dict[int, float] = {}
    for i, b in enumerate(all_bins):
        shade_dict[b] = float(shade_factors[i])

    # Compute fit quality against daily rows only (first n_daily rows).
    # The y vector also contains supplementary hourly rows which have a
    # different scale (single-hour kWh vs daily totals), so mixing them
    # inflates R² and deflates RMSE.  Reporting on daily rows matches
    # what the chart shows.
    y_pred = X @ shade_factors
    y_d     = y[:n_daily]
    y_pred_d = y_pred[:n_daily]
    rmse = float(np.sqrt(np.mean((y_d - y_pred_d) ** 2)))
    mae  = float(np.mean(np.abs(y_d - y_pred_d)))
    r2   = float(1 - np.sum((y_d - y_pred_d) ** 2) / np.sum((y_d - y_d.mean()) ** 2))

    print(f"\n  NNLS fit (daily totals, n={n_daily}): RMSE={rmse:.2f}, MAE={mae:.2f}, R2={r2:.3f}")

    # Compare with naive (uniform derating) — daily rows only
    naive_pred_d = X[:n_daily].sum(axis=1) * 0.80
    rmse_naive = float(np.sqrt(np.mean((y_d - naive_pred_d) ** 2)))
    print(f"  Naive (0.80): RMSE={rmse_naive:.2f}")

    # Build results DataFrame
    results = pd.DataFrame({
        "pos_bin": all_bins,
        "az_bin": [b // 100 for b in all_bins],
        "el_bin": [b % 100 for b in all_bins],
        "shade_factor": shade_factors,
        "total_predicted_kwh": X.sum(axis=0),
    })

    # Weight by how much energy each bin contributes
    results["energy_weight"] = results["total_predicted_kwh"] / results["total_predicted_kwh"].sum()
    results["weighted_shade"] = results["shade_factor"] * results["energy_weight"]

    return shade_dict, results


def smooth_shade_factors(
    shade_dict: dict[int, float],
    results: pd.DataFrame,
    iterations: int = 2,
) -> dict[int, float]:
    """
    Smooth the solved shade factors spatially by averaging each bin with
    its neighbours in (azimuth, elevation) space.

    The NNLS solver can produce isolated low-value bins at sun positions
    that are correlated with cloud cover in the training data (e.g. because
    clouds happened to be more common at a particular time of day during the
    training period).  These isolated lows cause spurious dips in the model
    output at specific times of day even on clear days.

    Smoothing pulls each bin toward its energy-weighted neighbourhood mean.
    Genuinely shaded zones (horizon obstructions) remain because they affect
    many spatially adjacent bins simultaneously; isolated noisy bins are
    suppressed.

    Parameters:
        shade_dict  -- pos_bin -> shade_factor from solve_shading()
        results     -- DataFrame with az_bin, el_bin, total_predicted_kwh
        iterations  -- smoothing passes; 2 is enough to remove single-bin dips

    Returns a new shade_dict with smoothed values.
    """
    # Energy weight lookup: used so well-constrained bins dominate
    energy = {int(row["pos_bin"]): float(row["total_predicted_kwh"])
              for _, row in results.iterrows()}

    current = dict(shade_dict)

    for _ in range(iterations):
        new_dict: dict[int, float] = {}
        for pos_bin, sf in current.items():
            az = pos_bin // 100
            el = pos_bin % 100

            # Accumulate weighted average over 3x3 neighbourhood in (az, el)
            w_sum = 0.0
            sf_sum = 0.0
            for daz in range(-1, 2):
                for del_ in range(-1, 2):
                    nb_az = (az + daz * AZ_BIN) % 360
                    nb_el = el + del_ * EL_BIN
                    if nb_el < 0:
                        continue
                    nb_bin = nb_az * 100 + nb_el
                    if nb_bin not in current:
                        continue
                    # Self gets 4× weight to limit how far each bin can move
                    w = energy.get(nb_bin, 1.0) * (4.0 if daz == 0 and del_ == 0 else 1.0)
                    sf_sum += current[nb_bin] * w
                    w_sum += w

            new_dict[pos_bin] = sf_sum / w_sum if w_sum > 0 else sf

        current = new_dict

    return current


def smooth_along_sun_path(
    shade_dict: dict[int, float],
    iterations: int = 10,
) -> dict[int, float]:
    """
    Remove single-hour dips from the shade factors by smoothing along the
    daily sun path rather than in (azimuth, elevation) grid space.

    Spatial grid smoothing cannot fix dips at noon or 10am because the sun's
    daily arc sweeps through non-adjacent grid bins between consecutive hours
    (especially in summer when the sun moves rapidly across the NE sky).
    This function treats the hourly bin sequence as a 1-D time series for
    each representative day of the year and raises any local minimum toward
    the mean of its path neighbours.

    Algorithm (iterated over representative days):
      For each consecutive triplet (prev_bin, curr_bin, next_bin) in the path:
        if shade[curr] < shade[prev]  AND  shade[curr] < shade[next]:
            shade[curr] = (shade[prev] + shade[next]) / 2
      Repeat until no dips remain or iterations exhausted.

    Parameters:
        shade_dict  -- pos_bin -> shade_factor (after spatial smoothing)
        iterations  -- max passes per representative day (default 10)

    Returns a new shade_dict with sun-path dips eliminated.
    """
    # One representative date every 5 days to cover all seasonal bin transitions
    rep_dates = pd.date_range("2026-01-01", periods=73, freq="5D")

    # Build the hourly sun-path (list of pos_bins above 5° elevation) for each date
    paths: list[list[int]] = []
    for d in rep_dates:
        day_ts = pd.DatetimeIndex([d + pd.Timedelta(hours=h) for h in range(24)])
        el, az = solar_position(day_ts)
        path: list[int] = []
        prev_pb = None
        for e, a in zip(el, az):
            if e <= 5.0:
                continue
            pb = int(a // AZ_BIN * AZ_BIN) * 100 + int(e // EL_BIN * EL_BIN)
            if pb in shade_dict and pb != prev_pb:  # skip repeated consecutive bins
                path.append(pb)
                prev_pb = pb
        if len(path) >= 3:
            paths.append(path)

    current = dict(shade_dict)

    for iteration in range(iterations):
        changed = 0
        for path in paths:
            for i in range(1, len(path) - 1):
                prev_b, curr_b, next_b = path[i - 1], path[i], path[i + 1]
                sf_prev = current.get(prev_b, 1.0)
                sf_curr = current.get(curr_b, 1.0)
                sf_next = current.get(next_b, 1.0)
                if sf_curr < sf_prev and sf_curr < sf_next:
                    current[curr_b] = (sf_prev + sf_next) / 2.0
                    changed += 1
        if changed == 0:
            print(f"  Sun-path smoothing converged after {iteration + 1} passes")
            break
    else:
        print(f"  Sun-path smoothing: {iterations} passes ({changed} residual dips)")

    return current


def apply_tree_shadow_correction(shade_dict: dict[int, float]) -> dict[int, float]:
    """
    Replace shade factors in the WNW tree shadow zone with geometry-based estimates.

    The NNLS solver learns shade factors from daily totals.  Because morning
    under-prediction and afternoon over-prediction cancel in daily kWh, the solver
    does not capture afternoon WNW shading: it assigns shade factors near 1.0 for
    sun positions at az=280-320°, el=15-30° when actual production is 34-64% of
    model.  This function overrides those values with a profile interpolated from
    direct hourly measurement.

    Physical basis (tree 10 m from panels, estimated height ≈ 11 m):
      - Angular height of tree top above panels: arctan((11-5)/10) ≈ 31°
      - Shading onset observed near el≈33° in autumn data
      - Below el=35°: shadow falls across an increasing fraction of the panels
      - At el≤5°: only diffuse sky remains (~10% of nameplate)
      - Azimuth range az=280-319° (WNW): covers the affected afternoon arc

    Calibration source: 17 days of actual vs. model comparison, Mar-Apr 2026.
    To be refined as data from other seasons (May-Sep) accumulates.

    Parameters:
        shade_dict -- pos_bin -> shade_factor after NNLS solve and smoothing

    Returns corrected shade_dict; bins outside the tree zone are unchanged.
    """
    el_pts = [p[0] for p in _TREE_SF_PROFILE]
    sf_pts = [p[1] for p in _TREE_SF_PROFILE]

    corrected = dict(shade_dict)
    n = 0
    for pos_bin in list(corrected.keys()):
        az_bin = pos_bin // 100
        el_bin = pos_bin % 100
        if az_bin not in _TREE_AZ_BINS:
            continue
        if el_bin >= _TREE_EL_ONSET:
            continue
        corrected[pos_bin] = float(np.interp(el_bin, el_pts, sf_pts))
        n += 1

    print(f"  Tree shadow: {n} bins corrected "
          f"(az {min(_TREE_AZ_BINS)}-{max(_TREE_AZ_BINS) + AZ_BIN - 1}deg, "
          f"el < {_TREE_EL_ONSET}deg)")
    return corrected


def apply_east_tree_shadow_correction(shade_dict: dict[int, float]) -> dict[int, float]:
    """
    Replace shade factors in the east tree shadow zone with a provisional
    geometry-based estimate (see _EAST_TREE_SF_PROFILE for the full
    calibration writeup).

    Unlike apply_tree_shadow_correction() (WNW), there is no hourly actual
    data for this zone yet -- solar_15min_history.csv has never recorded a
    day when the sun's morning azimuth reached az 80-140deg. The profile
    instead comes from a 2-parameter regression against real summer daily
    totals (enphase_daily.csv, 2023-2026), pooled by distance from the
    Dec-21 solstice. It is intentionally conservative and should be treated
    as provisional until real hourly data can pin it down the way the WNW
    zone was.

    Parameters:
        shade_dict -- pos_bin -> shade_factor after NNLS solve and smoothing

    Returns corrected shade_dict; bins outside the east tree zone are unchanged.
    """
    el_pts = [p[0] for p in _EAST_TREE_SF_PROFILE]
    sf_pts = [p[1] for p in _EAST_TREE_SF_PROFILE]

    corrected = dict(shade_dict)
    n = 0
    for pos_bin in list(corrected.keys()):
        az_bin = pos_bin // 100
        el_bin = pos_bin % 100
        if az_bin not in _EAST_TREE_AZ_BINS:
            continue
        if el_bin >= _EAST_TREE_EL_ONSET:
            continue
        corrected[pos_bin] = float(np.interp(el_bin, el_pts, sf_pts))
        n += 1

    print(f"  East tree shadow (provisional): {n} bins corrected "
          f"(az {min(_EAST_TREE_AZ_BINS)}-{max(_EAST_TREE_AZ_BINS) + AZ_BIN - 1}deg, "
          f"el < {_EAST_TREE_EL_ONSET}deg)")
    return corrected


def apply_observed_shade_overrides(shade_dict: dict[int, float]) -> dict[int, float]:
    """
    Apply per-(az_bin, el_bin) shade factor overrides from _OBSERVED_SHADE_OVERRIDES.

    These are empirical means of actual_kw / base_kw computed from
    solar_15min_history.csv for bins where the NNLS solver and tree
    profile both produce values inconsistent with the observed actuals.
    The override has the highest precedence: it replaces whatever value
    is currently in shade_dict for the listed bins.

    Bins not in the override table are unchanged.
    """
    corrected = dict(shade_dict)
    n = 0
    for (az_bin, el_bin), sf in _OBSERVED_SHADE_OVERRIDES.items():
        pos_bin = az_bin * 100 + el_bin
        prior = corrected.get(pos_bin)
        corrected[pos_bin] = float(sf)
        prior_str = f"{prior:.3f}" if prior is not None else "-"
        print(f"    az={az_bin:3d}, el={el_bin:2d}: {prior_str} -> {sf:.3f}")
        n += 1
    print(f"  Observed-shade overrides: {n} bins replaced")
    return corrected


def write_shade_grid(shade_dict: dict[int, float]) -> None:
    """
    Export the final (post-correction) per-(az_bin, el_bin) shade factor
    table to a small standalone CSV.

    Unlike data/calibrated_solar_hourly_v2.csv (a multi-year per-timestamp
    history, ~32k rows -- the wrong shape for a lookup table), this is just
    shade_dict itself: one row per sun-position bin actually seen in
    training, a few hundred rows at most. Meant to be copied onto an SD
    card for a microcontroller to load at boot and use to correct a raw
    Open-Meteo GTI forecast the same way apply_shading()/build_shade_lookup()
    do here -- same binning as everywhere else in this file: az_bin/el_bin
    are 10deg/5deg buckets, pos_bin = az_bin*100 + el_bin.
    """
    rows = sorted(
        (pos_bin // 100, pos_bin % 100, sf)
        for pos_bin, sf in shade_dict.items()
    )
    out_path = DATA_DIR / "shade_grid.csv"
    with open(out_path, "w", encoding="ascii", newline="") as f:
        f.write("az_bin,el_bin,shade_factor\n")
        for az_bin, el_bin, sf in rows:
            f.write(f"{az_bin},{el_bin},{sf:.4f}\n")
    print(f"  Wrote {len(rows)} (az_bin, el_bin) rows to {out_path.name}")


def build_morning_floor() -> pd.DataFrame:
    """
    Build per-(month, hour) empirical production floor from observed actuals.

    Open-Meteo's global_tilted_irradiance is systematically too low at sun
    elevations below ~10 deg, often by factors of 5-10x.  No multiplicative
    shade factor can fix this because actual production at low sun is roughly
    independent of OM's reported GTI on clear mornings (panels see real direct
    beam that OM is dramatically attenuating).  Instead we provide a lower
    bound on the prediction derived from observed mean production.

    Algorithm: join solar_15min_history.csv (hourly mean from 4 slots) with
    solar_history.csv (cloud_cover).  For each (month, hour) with actual_kw
    > 0.05:
      - If >=3 days had cloud_cover < 25, floor_kw = mean of those clear days
      - Else floor_kw = median across all days

    Output: data/morning_floor.csv with columns month, hour, floor_kw_clear,
    n_clear, n_total.  Floor is applied multiplicatively by cloud factor at
    runtime: effective = floor_kw_clear * (1 - cloud_cover/100 * 0.6).
    """
    sh = pd.read_csv(DATA_DIR / "solar_15min_history.csv", dtype={"date": str})
    rows = []
    for _, row in sh.iterrows():
        d = row["date"]
        for h in range(24):
            avg_kw = sum(row[f"slot_{h*4+j}"] for j in range(4)) / 4
            if avg_kw > 0.05:
                rows.append({"date_str": d, "hour": h, "actual_kw": avg_kw})
    actual = pd.DataFrame(rows)
    actual["month"] = actual["date_str"].str[5:7].astype(int)

    om = pd.read_csv(DATA_DIR / "solar_history.csv",
                     usecols=["timestamp", "cloud_cover"])
    om["timestamp"] = pd.to_datetime(om["timestamp"])
    om["date_str"] = om["timestamp"].dt.strftime("%Y-%m-%d")
    om["hour"] = om["timestamp"].dt.hour

    joined = actual.merge(om[["date_str", "hour", "cloud_cover"]],
                          on=["date_str", "hour"], how="left")

    floor_rows = []
    for (mon, hr), grp in joined.groupby(["month", "hour"]):
        clear = grp[grp["cloud_cover"].fillna(100) < 25]
        if len(clear) >= 3:
            floor_kw = float(clear["actual_kw"].mean())
            n_clear = len(clear)
        else:
            floor_kw = float(grp["actual_kw"].median())
            n_clear = 0
        floor_rows.append({"month": int(mon), "hour": int(hr),
                           "floor_kw_clear": round(floor_kw, 3),
                           "n_clear": n_clear, "n_total": len(grp)})

    floor_df = pd.DataFrame(floor_rows).sort_values(["month", "hour"])
    out_path = DATA_DIR / "morning_floor.csv"
    floor_df.to_csv(out_path, index=False)
    print(f"  Wrote {len(floor_df)} (month, hour) entries to {out_path.name}")
    return floor_df


def apply_shading(om_full: pd.DataFrame, shade_dict: dict[int, float],
                   morning_floor: "pd.DataFrame | None" = None) -> pd.DataFrame:
    """
    Apply shade factors to full hourly dataset.  Optionally floors the
    estimate using the empirical (month, hour) production table from
    build_morning_floor() to compensate for OM's GTI underestimation at
    low sun elevations.
    """
    om_full = om_full.copy()
    om_full["gti"] = om_full["global_tilted_irradiance"].fillna(0)
    om_full["base_kw"] = SYSTEM_KW * om_full["gti"] / 1000.0

    ts_index = pd.DatetimeIndex(om_full["timestamp"])
    elevation, azimuth = solar_position(ts_index)
    om_full["sun_elevation"] = elevation
    om_full["sun_azimuth"] = azimuth
    om_full["az_bin"] = (om_full["sun_azimuth"] // AZ_BIN * AZ_BIN).astype(int)
    om_full["el_bin"] = (om_full["sun_elevation"] // EL_BIN * EL_BIN).astype(int)
    om_full["pos_bin"] = om_full["az_bin"] * 100 + om_full["el_bin"]

    # Look up shade factor for each hour
    om_full["shade_factor"] = om_full["pos_bin"].map(shade_dict).fillna(0.85)

    # Calibrated estimate
    om_full["est_kw_shading"] = om_full["base_kw"] * om_full["shade_factor"]

    # Apply empirical morning floor (compensates for OM GTI bias at low sun).
    # Only takes effect where the floor exceeds the model estimate AND the sun
    # is up (elevation > 0).  Cloud factor scales floor between 0.4x (overcast)
    # and 1.0x (clear).
    if morning_floor is not None and not morning_floor.empty:
        om_full["month"] = ts_index.month
        om_full["hour"] = ts_index.hour
        floor_map = morning_floor.set_index(["month", "hour"])["floor_kw_clear"].to_dict()
        keys = list(zip(om_full["month"].astype(int), om_full["hour"].astype(int)))
        om_full["floor_clear"] = [floor_map.get(k, 0.0) for k in keys]
        cc = om_full["cloud_cover"].fillna(50).clip(0, 100) if "cloud_cover" in om_full.columns else 50
        cloud_factor = (1 - cc / 100 * 0.6).clip(0.4, 1.0)
        om_full["floor_effective"] = om_full["floor_clear"] * cloud_factor
        # Only floor when sun is up
        sun_up = om_full["sun_elevation"] > 0
        om_full.loc[sun_up, "est_kw_shading"] = om_full.loc[sun_up, ["est_kw_shading", "floor_effective"]].max(axis=1)

    om_full["est_kw_shading"] = om_full["est_kw_shading"].clip(lower=0, upper=SYSTEM_KW)

    # Old estimate for comparison
    om_full["est_kw_old"] = om_full["base_kw"] * 0.80

    out = om_full[["timestamp", "gti", "sun_elevation", "sun_azimuth",
                    "shade_factor", "est_kw_old", "est_kw_shading"]].copy()
    out.columns = ["timestamp", "gti_wm2", "sun_elevation", "sun_azimuth",
                   "shade_factor", "est_kw_old", "est_kw_shading"]

    return out


def make_charts(
    results: pd.DataFrame,
    hourly: pd.DataFrame,
    enphase: pd.DataFrame,
) -> None:
    """Generate shading model charts."""
    CHART_DIR.mkdir(parents=True, exist_ok=True)

    fig = plt.figure(figsize=(18, 14))

    # ── 1. Polar shading map (the star chart) ──
    ax_polar = fig.add_subplot(2, 3, 1, projection="polar")

    # Filter to bins with meaningful energy
    sig = results[results["total_predicted_kwh"] > 10].copy()

    # Convert azimuth to radians (0=North, clockwise)
    # matplotlib polar: 0=right(East), counter-clockwise
    # We want 0=North, clockwise, so: theta = pi/2 - azimuth_rad
    az_rad = np.radians(sig["az_bin"] + AZ_BIN / 2)
    # Convert to matplotlib convention
    theta = np.pi / 2 - az_rad

    # Elevation as radius (higher elevation = closer to centre)
    r = 90 - (sig["el_bin"] + EL_BIN / 2)

    # Color by shade factor
    norm = mcolors.Normalize(vmin=0.5, vmax=1.2)
    cmap = plt.cm.RdYlGn  # type: ignore[attr-defined]

    scatter = ax_polar.scatter(theta, r, c=sig["shade_factor"],
                               s=sig["energy_weight"] * 3000 + 20,
                               cmap=cmap, norm=norm, alpha=0.8,
                               edgecolors="white", linewidth=0.5)
    ax_polar.set_theta_zero_location("N")
    ax_polar.set_theta_direction(-1)  # clockwise
    ax_polar.set_ylim(0, 90)
    ax_polar.set_yticks([0, 20, 40, 60, 80])
    ax_polar.set_yticklabels(["90°", "70°", "50°", "30°", "10°"], fontsize=6)
    ax_polar.set_title("A — Shading Map (polar)\n"
                       "Centre=zenith, edge=horizon\n"
                       "Green=clear, Red=shaded",
                       fontsize=9, fontweight="bold", pad=20)
    plt.colorbar(scatter, ax=ax_polar, shrink=0.6, label="Shade Factor",
                 pad=0.1)

    # ── 2. Rectangular shade map (azimuth × elevation) ──
    ax_rect = fig.add_subplot(2, 3, 2)
    tufte(ax_rect)

    # Pivot to grid
    pivot = results.pivot_table(index="el_bin", columns="az_bin",
                                values="shade_factor", aggfunc="mean")
    # Only show populated range
    az_range = sorted(results[results["total_predicted_kwh"] > 5]["az_bin"].unique())
    el_range = sorted(results[results["total_predicted_kwh"] > 5]["el_bin"].unique())
    if len(az_range) > 0 and len(el_range) > 0:
        pivot_trimmed = pivot.reindex(index=el_range, columns=az_range)

        im = ax_rect.imshow(pivot_trimmed.values, aspect="auto",
                            cmap="RdYlGn", vmin=0.5, vmax=1.2,
                            origin="lower", interpolation="nearest")
        ax_rect.set_xticks(range(len(az_range)))
        ax_rect.set_xticklabels([f"{a}°" for a in az_range], fontsize=6,
                                rotation=45)
        ax_rect.set_yticks(range(len(el_range)))
        ax_rect.set_yticklabels([f"{e}°" for e in el_range], fontsize=7)
        ax_rect.set_xlabel("Azimuth (°from N)")
        ax_rect.set_ylabel("Elevation (°)")
        plt.colorbar(im, ax=ax_rect, shrink=0.8, label="Shade Factor")

    ax_rect.set_title("B — Shade Factor Grid\n"
                      "(azimuth × elevation)", fontsize=9, fontweight="bold")

    # ── 3. Shade factor vs azimuth (averaged over elevations) ──
    ax_az = fig.add_subplot(2, 3, 3)
    tufte(ax_az)

    # Weight by energy contribution
    weighted = results[results["total_predicted_kwh"] > 5].copy()
    az_profile = weighted.groupby("az_bin").apply(
        lambda g: np.average(g["shade_factor"],
                             weights=g["total_predicted_kwh"])
    )
    ax_az.bar(az_profile.index, az_profile.values, width=AZ_BIN * 0.8,
              color=["#4CAF50" if v >= 0.85 else "#C44E52" for v in az_profile.values],
              alpha=0.7, edgecolor="white")
    ax_az.axhline(0.85, color=C_LIGHT, linewidth=0.5, linestyle="--")
    ax_az.set_xlabel("Azimuth (° from North)")
    ax_az.set_ylabel("Avg Shade Factor (energy-weighted)")
    ax_az.set_title("C — Shade Factor by Azimuth\n"
                    "(E=90°, N=0/360°, W=270°, S=180°)",
                    fontsize=9, fontweight="bold")

    # Annotate directions
    for az, label in [(0, "N"), (90, "E"), (180, "S"), (270, "W")]:
        if az in az_profile.index or any(abs(az_profile.index - az) < AZ_BIN):
            ax_az.axvline(az, color=C_LIGHT, linewidth=0.3, alpha=0.5)
            ax_az.text(az, ax_az.get_ylim()[1], label, ha="center",
                       fontsize=8, color=C_TEXT)

    # ── 4. Shade factor vs elevation ──
    ax_el = fig.add_subplot(2, 3, 4)
    tufte(ax_el)

    el_profile = weighted.groupby("el_bin").apply(
        lambda g: np.average(g["shade_factor"],
                             weights=g["total_predicted_kwh"])
    )
    ax_el.bar(el_profile.index, el_profile.values, width=EL_BIN * 0.8,
              color=["#4CAF50" if v >= 0.85 else "#C44E52" for v in el_profile.values],
              alpha=0.7, edgecolor="white")
    ax_el.axhline(0.85, color=C_LIGHT, linewidth=0.5, linestyle="--")
    ax_el.set_xlabel("Sun Elevation (°)")
    ax_el.set_ylabel("Avg Shade Factor (energy-weighted)")
    ax_el.set_title("D — Shade Factor by Elevation\n"
                    "(low angles = near horizon)",
                    fontsize=9, fontweight="bold")

    # ── 5. Daily comparison: old vs shading-corrected ──
    ax_ts = fig.add_subplot(2, 3, 5)
    tufte(ax_ts)

    # Compute daily predictions
    hourly["date"] = pd.to_datetime(hourly["timestamp"]).dt.date
    daily_pred = hourly.groupby("date").agg(
        old_kwh=("est_kw_old", "sum"),
        shading_kwh=("est_kw_shading", "sum"),
    )
    daily_pred.index = pd.to_datetime(daily_pred.index)

    merged = enphase.join(daily_pred, how="inner")
    merged = merged[merged["production_kwh"] > 1]

    # 14-day rolling residuals
    res_old = merged["production_kwh"] - merged["old_kwh"]
    res_new = merged["production_kwh"] - merged["shading_kwh"]

    roll_old = res_old.rolling(28, min_periods=7).mean()
    roll_new = res_new.rolling(28, min_periods=7).mean()

    ax_ts.plot(merged.index, roll_old, color="#AAAAAA", linewidth=1.5,
               label=f"Old 0.80 (RMSE={np.sqrt((res_old**2).mean()):.1f})",
               alpha=0.7)
    ax_ts.plot(merged.index, roll_new, color="#4CAF50", linewidth=1.5,
               label=f"Shading model (RMSE={np.sqrt((res_new**2).mean()):.1f})")
    ax_ts.axhline(0, color=C_LIGHT, linewidth=0.5)
    ax_ts.set_ylabel("Residual kWh (actual-predicted)")
    ax_ts.set_title("E — Residual Over Time (28-day avg)",
                    fontsize=9, fontweight="bold")
    ax_ts.legend(fontsize=7, frameon=False)

    # ── 6. Scatter: shading model ──
    ax_sc = fig.add_subplot(2, 3, 6)
    tufte(ax_sc)

    ax_sc.scatter(merged["shading_kwh"], merged["production_kwh"],
                  s=5, alpha=0.2, color="#4CAF50", edgecolors="none")
    mx = max(merged["shading_kwh"].max(), merged["production_kwh"].max()) * 1.05
    ax_sc.plot([0, mx], [0, mx], "--", color=C_LIGHT, linewidth=1)
    ax_sc.set_xlabel("Predicted (kWh)")
    ax_sc.set_ylabel("Actual (kWh)")

    r2 = 1 - np.sum(res_new**2) / np.sum((merged["production_kwh"] - merged["production_kwh"].mean())**2)
    ax_sc.set_title(f"F — Shading Model Scatter\n"
                    f"R²={r2:.3f}, RMSE={np.sqrt((res_new**2).mean()):.1f}",
                    fontsize=9, fontweight="bold")
    ax_sc.set_xlim(0, mx)
    ax_sc.set_ylim(0, mx)
    ax_sc.set_aspect("equal")

    fig.suptitle("Solar Shading Model — Per-Angle Calibration\n"
                 "Back-solved horizon mask from 1,005 days of Enphase vs Open-Meteo",
                 fontsize=13, fontweight="bold")
    fig.tight_layout()
    fig.savefig(CHART_DIR / "solar_shading_model.png", dpi=150,
                bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print("  solar_shading_model.png")


def write_report(
    results: pd.DataFrame,
    hourly: pd.DataFrame,
    enphase: pd.DataFrame,
) -> None:
    """Write shading model report."""
    # Compute daily stats
    hourly_copy = hourly.copy()
    hourly_copy["date"] = pd.to_datetime(hourly_copy["timestamp"]).dt.date
    daily = hourly_copy.groupby("date").agg(
        old_kwh=("est_kw_old", "sum"),
        shading_kwh=("est_kw_shading", "sum"),
    )
    daily.index = pd.to_datetime(daily.index)
    merged = enphase.join(daily, how="inner")
    merged = merged[merged["production_kwh"] > 1]

    res_old = merged["production_kwh"] - merged["old_kwh"]
    res_new = merged["production_kwh"] - merged["shading_kwh"]

    rmse_old = float(np.sqrt((res_old**2).mean()))
    rmse_new = float(np.sqrt((res_new**2).mean()))
    mae_old  = float(res_old.abs().mean())
    mae_new  = float(res_new.abs().mean())
    bias_old = float(res_old.mean())
    bias_new = float(res_new.mean())
    ss_tot   = float(((merged["production_kwh"] - merged["production_kwh"].mean())**2).sum())
    r2_old   = float(1 - (res_old**2).sum() / ss_tot)
    r2_new   = float(1 - (res_new**2).sum() / ss_tot)

    # Seasonal breakdown (same definitions as solar_prediction_pipeline.md)
    month = merged.index.month
    seasons = {
        "Summer (Nov-Feb)":        merged[month.isin([11, 12, 1, 2])],
        "Equinox (Mar-Apr/Sep-Oct)": merged[month.isin([3, 4, 9, 10])],
        "Winter (May-Aug)":        merged[month.isin([5, 6, 7, 8])],
    }

    # Key shade factors by direction
    sig = results[results["total_predicted_kwh"] > 5].copy()
    directions = {
        "North (350-10 deg)": sig[sig["az_bin"].isin([350, 0, 10])],
        "NE (20-70 deg)": sig[(sig["az_bin"] >= 20) & (sig["az_bin"] <= 70)],
        "East (80-100 deg)": sig[sig["az_bin"].isin([80, 90, 100])],
        "SE (110-160 deg)": sig[(sig["az_bin"] >= 110) & (sig["az_bin"] <= 160)],
        "South (170-190 deg)": sig[sig["az_bin"].isin([170, 180, 190])],
        "SW (200-250 deg)": sig[(sig["az_bin"] >= 200) & (sig["az_bin"] <= 250)],
        "West (260-280 deg)": sig[sig["az_bin"].isin([260, 270, 280])],
        "NW (290-340 deg)": sig[(sig["az_bin"] >= 290) & (sig["az_bin"] <= 340)],
    }

    lines = [
        "# Solar Shading Model",
        "",
        f"**Method**: Back-solve per-angle shade factors from {len(merged)} days "
        f"of Enphase daily production vs Open-Meteo hourly GTI predictions",
        "",
        f"**Resolution**: {AZ_BIN} deg azimuth x {EL_BIN} deg elevation bins",
        f"**Sun position bins**: {len(results)} total, "
        f"{len(sig)} with significant energy",
        "",
        "## Model Accuracy",
        "",
        "| Model | RMSE (kWh) | MAE (kWh) | Bias (kWh) | R^2 |",
        "|-------|----------:|---------:|-----------:|----:|",
        f"| Old (uniform 0.80) | {rmse_old:.2f} | {mae_old:.2f} | {bias_old:+.2f} | {r2_old:.3f} |",
        f"| **Shading model** | **{rmse_new:.2f}** | **{mae_new:.2f}** | **{bias_new:+.2f}** | **{r2_new:.3f}** |",
        f"| Improvement | {(1 - rmse_new/rmse_old)*100:.1f}% | "
        f"{(1 - mae_new/mae_old)*100:.1f}% | "
        f"{abs(bias_new):.2f} vs {abs(bias_old):.2f} | -- |",
        "",
        "Bias is mean(actual - predicted): positive = model under-predicts, "
        "negative = over-predicts.",
        "",
        "## Seasonal Accuracy (Shading Model)",
        "",
        "| Season | Days | RMSE (kWh) | MAE (kWh) | Bias (kWh) |",
        "|--------|-----:|----------:|---------:|-----------:|",
    ]

    for season_name, subset in seasons.items():
        if len(subset) < 5:
            continue
        s_res = subset["production_kwh"] - subset["shading_kwh"]
        s_rmse = float(np.sqrt((s_res**2).mean()))
        s_mae  = float(s_res.abs().mean())
        s_bias = float(s_res.mean())
        lines.append(f"| {season_name} | {len(subset)} | {s_rmse:.2f} | "
                     f"{s_mae:.2f} | {s_bias:+.2f} |")

    lines.extend([
        "",
        "## Shade Factors by Direction",
        "",
        "| Direction | Avg Shade Factor | Energy Contribution |",
        "|-----------|:----------------:|:-------------------:|",
    ])

    for direction, subset in directions.items():
        if len(subset) > 0:
            avg_sf = np.average(subset["shade_factor"],
                                weights=subset["total_predicted_kwh"])
            energy_pct = subset["total_predicted_kwh"].sum() / sig["total_predicted_kwh"].sum() * 100
            lines.append(f"| {direction} | {avg_sf:.3f} | {energy_pct:.1f}% |")

    # Find shading hot-spots (lowest shade factors with significant energy)
    shaded = sig[sig["shade_factor"] < 0.7].sort_values("total_predicted_kwh",
                                                          ascending=False)
    if len(shaded) > 0:
        lines.extend([
            "",
            "## Shading Hot-Spots (shade factor < 0.70)",
            "",
            "| Azimuth | Elevation | Shade Factor | Energy (kWh) |",
            "|--------:|----------:|:------------:|:------------:|",
        ])
        for _, row in shaded.head(15).iterrows():
            az_label = f"{int(row['az_bin'])}-{int(row['az_bin']) + AZ_BIN} deg"
            el_label = f"{int(row['el_bin'])}-{int(row['el_bin']) + EL_BIN} deg"
            lines.append(f"| {az_label} | {el_label} | "
                         f"{row['shade_factor']:.3f} | "
                         f"{row['total_predicted_kwh']:.0f} |")

    # Find clear zones (highest shade factors)
    clear = sig[sig["shade_factor"] > 0.95].sort_values("total_predicted_kwh",
                                                          ascending=False)
    if len(clear) > 0:
        lines.extend([
            "",
            "## Clear Sky Zones (shade factor > 0.95)",
            "",
            "| Azimuth | Elevation | Shade Factor | Energy (kWh) |",
            "|--------:|----------:|:------------:|:------------:|",
        ])
        for _, row in clear.head(10).iterrows():
            az_label = f"{int(row['az_bin'])}-{int(row['az_bin']) + AZ_BIN} deg"
            el_label = f"{int(row['el_bin'])}-{int(row['el_bin']) + EL_BIN} deg"
            lines.append(f"| {az_label} | {el_label} | "
                         f"{row['shade_factor']:.3f} | "
                         f"{row['total_predicted_kwh']:.0f} |")

    lines.extend([
        "",
        "## Interpretation",
        "",
        "- **Shade factor < 0.80**: Significant shading -- trees, hills, or "
        "buildings block the sun at this position",
        "- **Shade factor 0.80-0.90**: Expected derating (inverter loss, "
        "temperature, wiring, soiling)",
        "- **Shade factor > 0.95**: Panels perform at or above OM prediction "
        "(possible albedo/reflection boost)",
        "",
        "## Output",
        "",
        f"Calibrated hourly estimates: `data/calibrated_solar_hourly_v2.csv` "
        f"({len(hourly)} hours)",
        "",
        "## Charts",
        "",
        "- `charts/solar_shading_model.png` -- Polar shading map, grid, "
        "azimuth/elevation profiles, residuals, scatter",
    ])

    report_path = ROOT_DIR / "docs" / "solar_shading_stats.md"
    report_path.write_text("\n".join(lines))
    print(f"  Report: {report_path}")


def main() -> None:
    import warnings as _warnings
    _warn_log = ROOT_DIR / "docs" / "charts" / "quarterly_update.log"
    def _warn_to_log(message, category, filename, lineno, file=None, line=None):
        with open(_warn_log, "a", encoding="ascii") as _lf:
            _lf.write(f"WARNING [{category.__name__}]: {message}  ({filename}:{lineno})\n")
    _warnings.showwarning = _warn_to_log

    print("Loading data and computing solar positions...")
    enphase, om, hourly_actual = load_and_prepare()
    print(f"  {len(enphase)} Enphase days, {len(om)} OM daylight hours")

    print("\nSolving for shade factors (NNLS regression)...")
    shade_dict, results = solve_shading(enphase, om, hourly_actual)

    print("\nSmoothing shade factors (spatial neighbourhood average)...")
    shade_dict = smooth_shade_factors(shade_dict, results, iterations=2)
    print("Smoothing shade factors (sun-path dip removal)...")
    shade_dict = smooth_along_sun_path(shade_dict, iterations=10)
    print("\nApplying tree shadow correction (WNW, geometry from autumn data)...")
    shade_dict = apply_tree_shadow_correction(shade_dict)
    print("\nApplying east tree shadow correction (provisional, season-pooled)...")
    shade_dict = apply_east_tree_shadow_correction(shade_dict)
    print("\nApplying empirical (az,el) shade overrides...")
    shade_dict = apply_observed_shade_overrides(shade_dict)
    # Update results DataFrame to reflect smoothed + corrected values
    results["shade_factor"] = results["pos_bin"].map(shade_dict)

    print("\nExporting shade grid for SD-card / microcontroller use...")
    write_shade_grid(shade_dict)

    # Print key findings
    sig = results[results["total_predicted_kwh"] > 5]
    print(f"\n  Energy-weighted mean shade factor: "
          f"{np.average(sig['shade_factor'], weights=sig['total_predicted_kwh']):.3f}")

    best = sig.nlargest(5, "shade_factor")
    worst = sig[sig["shade_factor"] > 0].nsmallest(5, "total_predicted_kwh")
    most_shaded = sig.nsmallest(5, "shade_factor")

    print("\n  Most shaded positions:")
    for _, row in most_shaded.iterrows():
        print(f"    Az {int(row['az_bin']):3d}deg, El {int(row['el_bin']):2d}deg: "
              f"shade={row['shade_factor']:.3f} "
              f"({row['total_predicted_kwh']:.0f} kWh)")

    print("\n  Clearest positions:")
    for _, row in best.iterrows():
        print(f"    Az {int(row['az_bin']):3d}deg, El {int(row['el_bin']):2d}deg: "
              f"shade={row['shade_factor']:.3f} "
              f"({row['total_predicted_kwh']:.0f} kWh)")

    print("\nBuilding empirical morning-floor table...")
    morning_floor = build_morning_floor()

    print("\nApplying shading model to full hourly dataset...")
    om_full = pd.read_csv(DATA_DIR / "solar_history.csv")
    om_full["timestamp"] = pd.to_datetime(om_full["timestamp"])
    hourly = apply_shading(om_full, shade_dict, morning_floor)

    out_path = DATA_DIR / "calibrated_solar_hourly_v2.csv"
    hourly.to_csv(out_path, index=False, float_format="%.3f")
    print(f"  Written {len(hourly)} rows to {out_path}")

    print("\nGenerating charts...")
    make_charts(results, hourly, enphase)

    print("\nWriting report...")
    write_report(results, hourly, enphase)

    print("\nDone!")


if __name__ == "__main__":
    import traceback as _tb
    try:
        main()
    except Exception:
        print(f"UNHANDLED ERROR in {Path(__file__).name}:")
        print(_tb.format_exc())
        raise SystemExit(1)
