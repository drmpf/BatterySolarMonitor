#!/usr/bin/env python3
"""
Analyse the tree shadow zone calibration data and accumulate per-day implied
shade factors for the WNW (west-northwest) shadow zone.

For each day that has actual hourly production data and the sun was in the
WNW tree shadow zone (az 280-319°, el 0-49°), computes the implied shade
factor (actual_kw / base_kw) and writes one row per qualifying day+hour to
data/tree_shadow_calibration.csv.

This accumulates evidence over time so the _TREE_SF_PROFILE constants in
solar_shading_model.py can be refined with more seasonal coverage as data
becomes available.

Reads:
  data/calibrated_solar_hourly_v2.csv   -- gti_wm2, sun_elevation, sun_azimuth
  data/solar_15min_history.csv          -- 96 15-min slots/day, averaged to
                                            hourly actual_kw here

Writes:
  data/tree_shadow_calibration.csv      -- per-hour calibration observations

Prints:
  Summary table of per-el_bin mean implied shade factors vs current model
  (useful for inspecting whether _TREE_SF_PROFILE needs updating)

Usage:
  python analyse_tree_shadow.py

Migrated to BatterySolarMonitorUpdate 2026-09-10: the original
(dailyPrediction/analyse_tree_shadow.py) read
data/solar_hourly_forecast_vs_actual.csv, written by
dailyPrediction/plot_today_solar.py's daily HTA run -- not migrated here.
That file's (date, hour, actual_kw) shape is otherwise identical to what
solar_shading_model.py's own load_and_prepare() and every diag_*.py script
here already derive from solar_15min_history.csv (mean of the 4 15-min
slots in each hour), so load_merged() below builds the same thing from that
file instead -- no capability lost, just a different (already-available)
source for the same data.
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pandas as pd

PROJECT_DIR = Path(__file__).parent          # py_programs/
ROOT_DIR    = PROJECT_DIR.parent             # BatterySolarMonitorUpdate/
DATA_DIR    = ROOT_DIR / "data"

sys.path.insert(0, str(PROJECT_DIR))
from solar_shading_model import SYSTEM_KW  # noqa: E402 -- see that module's own docstring

# WNW tree shadow zone — must match solar_shading_model.py constants
TREE_AZ_BINS = {280, 290, 300, 310}   # az_bin values (floor(az/10)*10)
TREE_EL_ONSET = 50                    # el_bin: no tree shading at or above this

AZ_BIN = 10   # bin resolution — must match solar_shading_model.py
EL_BIN = 5


def _hourly_actual_from_15min() -> pd.DataFrame:
    """
    Build a (date, hour, actual_kw) table from solar_15min_history.csv --
    same mean-of-4-slots transform solar_shading_model.py's own
    load_and_prepare() and every diag_*.py script here already use.
    """
    sh = pd.read_csv(DATA_DIR / "solar_15min_history.csv", dtype={"date": str})
    rows = []
    for _, row in sh.iterrows():
        d = row["date"]
        for h in range(24):
            avg_kw = sum(row[f"slot_{h*4+j}"] for j in range(4)) / 4
            if avg_kw > 0.01:
                rows.append({"date": d, "hour": h, "actual_kw": avg_kw})
    return pd.DataFrame(rows)


def load_merged() -> pd.DataFrame:
    """
    Load and merge calibrated hourly data with actual production.

    Returns a DataFrame restricted to daylight hours (gti_wm2 > 10) where
    actual production is available.  Adds az_bin, el_bin columns.

    Returns:
        DataFrame with columns: timestamp, gti_wm2, sun_elevation, sun_azimuth,
        shade_factor, base_kw, actual_kw, az_bin, el_bin
    """
    calib = pd.read_csv(
        DATA_DIR / "calibrated_solar_hourly_v2.csv",
        usecols=["timestamp", "gti_wm2", "sun_elevation", "sun_azimuth", "shade_factor"],
    )
    calib["timestamp"] = pd.to_datetime(calib["timestamp"])

    actual = _hourly_actual_from_15min()
    actual["timestamp"] = (
        pd.to_datetime(actual["date"]) + pd.to_timedelta(actual["hour"], unit="h")
    )
    actual = actual[["timestamp", "actual_kw"]]

    merged = pd.merge(calib, actual, on="timestamp", how="inner")
    merged = merged[merged["actual_kw"].notna() & (merged["gti_wm2"] > 10)].copy()

    merged["base_kw"]  = (SYSTEM_KW * merged["gti_wm2"] / 1000.0).clip(0.1, SYSTEM_KW)
    merged["az_bin"]   = (merged["sun_azimuth"]   // AZ_BIN * AZ_BIN).astype(int)
    merged["el_bin"]   = (merged["sun_elevation"]  // EL_BIN * EL_BIN).astype(int)

    return merged.sort_values("timestamp").reset_index(drop=True)


def build_calibration_rows(merged: pd.DataFrame) -> pd.DataFrame:
    """
    Extract hourly rows where the sun is in the WNW tree shadow zone and
    actual production was recorded.  Computes implied_sf = actual_kw / base_kw.

    Only rows with el_bin in [0, TREE_EL_ONSET) and az_bin in TREE_AZ_BINS
    are included.

    Parameters:
        merged -- merged DataFrame from load_merged()

    Returns:
        DataFrame with columns: date, hour, az_bin, el_bin, sun_elevation,
        sun_azimuth, gti_wm2, base_kw, actual_kw, shade_factor, implied_sf
    """
    mask = (
        merged["az_bin"].isin(TREE_AZ_BINS)
        & (merged["el_bin"] >= 0)
        & (merged["el_bin"] < TREE_EL_ONSET)
    )
    rows = merged[mask].copy()
    rows["implied_sf"] = (rows["actual_kw"] / rows["base_kw"]).clip(0, 2.0)
    rows["date"]  = rows["timestamp"].dt.date.astype(str)
    rows["hour"]  = rows["timestamp"].dt.hour
    out_cols = [
        "date", "hour", "az_bin", "el_bin",
        "sun_elevation", "sun_azimuth",
        "gti_wm2", "base_kw", "actual_kw",
        "shade_factor", "implied_sf",
    ]
    return rows[out_cols].reset_index(drop=True)


def update_calibration_csv(new_rows: pd.DataFrame) -> pd.DataFrame:
    """
    Merge new calibration rows with any previously saved data and write
    the combined result back to data/tree_shadow_calibration.csv.

    Rows are keyed on (date, hour) — duplicates from prior runs are replaced
    by the latest values (in case the model shade_factor was updated).

    Parameters:
        new_rows -- DataFrame from build_calibration_rows()

    Returns:
        Combined DataFrame written to CSV.
    """
    out_path = DATA_DIR / "tree_shadow_calibration.csv"

    if out_path.exists():
        existing = pd.read_csv(out_path)
        # Drop rows that now appear in new_rows (stale model shade_factor)
        key = ["date", "hour"]
        existing = existing[
            ~existing.set_index(key).index.isin(new_rows.set_index(key).index)
        ]
        combined = pd.concat([existing, new_rows], ignore_index=True)
    else:
        combined = new_rows

    combined = combined.sort_values(["date", "hour"]).reset_index(drop=True)
    combined.to_csv(out_path, index=False)
    return combined


def print_summary(combined: pd.DataFrame) -> None:
    """
    Print a per-el_bin summary comparing the mean implied shade factor
    (actual/base_kw) against the mean model shade factor currently stored
    in calibrated_solar_hourly_v2.csv.

    A large gap between implied_sf and model_sf indicates the
    _TREE_SF_PROFILE in solar_shading_model.py may need updating.

    Parameters:
        combined -- full calibration DataFrame (all accumulated rows)
    """
    grp = (
        combined.groupby("el_bin")
        .agg(
            n=("implied_sf", "count"),
            mean_implied=("implied_sf", "mean"),
            median_implied=("implied_sf", "median"),
            min_implied=("implied_sf", "min"),
            max_implied=("implied_sf", "max"),
            mean_model_sf=("shade_factor", "mean"),
            mean_el=("sun_elevation", "mean"),
        )
        .reset_index()
    )

    print("\nTree shadow calibration summary (WNW zone, az 280-319deg, el < 50deg):")
    print(f"  Total observations: {len(combined)}")
    print(f"  Date range: {combined['date'].min()} to {combined['date'].max()}")
    print()
    print(f"{'el_bin':>6}  {'n':>4}  {'mean_el':>7}  "
          f"{'mean_implied':>12}  {'median':>7}  {'min':>6}  {'max':>6}  "
          f"{'model_sf':>8}  {'gap':>6}")
    print("  " + "-" * 78)
    for _, row in grp.iterrows():
        gap = row["mean_implied"] - row["mean_model_sf"]
        flag = "  **" if abs(gap) > 0.15 else ""
        print(
            f"  {int(row['el_bin']):>4}  {int(row['n']):>4}  "
            f"{row['mean_el']:>7.1f}  "
            f"{row['mean_implied']:>12.3f}  "
            f"{row['median_implied']:>7.3f}  "
            f"{row['min_implied']:>6.3f}  "
            f"{row['max_implied']:>6.3f}  "
            f"{row['mean_model_sf']:>8.3f}  "
            f"{gap:>+6.3f}{flag}"
        )
    print()
    print("  gap = mean_implied - model_sf.  |gap| > 0.15 flagged with **")
    print("  Update _TREE_SF_PROFILE in solar_shading_model.py when n >= 5 and gap is large.")


def suggest_updates(combined: pd.DataFrame) -> None:
    """
    Compare accumulated calibration data against the current model profile and print
    suggested updates to _TREE_SF_PROFILE and _TREE_EL_ONSET in solar_shading_model.py.

    For each elevation bin with n >= 5 calibration observations and |gap| > 0.15
    (where gap = mean_implied - mean_model_sf), the shade factor is updated to the
    mean implied value.  If the highest qualifying bin is at or above
    _TREE_EL_ONSET - EL_BIN, the onset threshold is raised by one bin to capture
    the evidence that shading extends higher.

    Prints the complete updated constants ready to paste into solar_shading_model.py,
    or a "no changes needed" message if all el_bins are within tolerance.

    Parameters:
        combined -- full calibration DataFrame from update_calibration_csv()
    """
    try:
        from solar_shading_model import (  # type: ignore[import]
            _TREE_AZ_BINS as cur_az_bins,
            _TREE_SF_PROFILE as cur_profile,
            _TREE_EL_ONSET as cur_onset,
        )
    except ImportError as exc:
        print(f"\n  Cannot import solar_shading_model to check current profile: {exc}")
        return

    grp = (
        combined.groupby("el_bin")
        .agg(
            n=("implied_sf", "count"),
            mean_implied=("implied_sf", "mean"),
            mean_model_sf=("shade_factor", "mean"),
        )
        .reset_index()
    )

    # Find el_bins that qualify for an update (n >= 5 and |gap| > 0.15)
    qualifying: dict[int, tuple[float, int, float]] = {}
    for _, row in grp.iterrows():
        el_bin = int(row["el_bin"])
        n = int(row["n"])
        gap = float(row["mean_implied"] - row["mean_model_sf"])
        if n >= 5 and abs(gap) > 0.15:
            qualifying[el_bin] = (float(row["mean_implied"]), n, gap)

    print()
    if not qualifying:
        print("  No changes needed to _TREE_AZ_BINS, _TREE_SF_PROFILE or _TREE_EL_ONSET.")
        return

    # Build updated profile from current constants
    orig_dict: dict[int, float] = dict(cur_profile)
    new_dict: dict[int, float] = dict(orig_dict)
    for el_bin, (mean_implied, _, _) in qualifying.items():
        new_dict[el_bin] = round(mean_implied, 3)

    # Raise onset if the highest qualifying bin is within one step of the current onset
    max_qual_el = max(qualifying.keys())
    new_onset = cur_onset
    if max_qual_el >= cur_onset - EL_BIN:
        new_onset = cur_onset + EL_BIN
        if new_onset not in new_dict:
            new_dict[new_onset] = 1.00
    new_profile = sorted(new_dict.items())

    # Print suggested updates
    qual_els = ", ".join(f"el_bin={e}" for e in sorted(qualifying))
    print(f"  Changes needed ({qual_els}).")
    print("  Suggested updates for solar_shading_model.py:")
    print()
    az_sorted = sorted(cur_az_bins)
    print(f"_TREE_AZ_BINS = {{{', '.join(str(a) for a in az_sorted)}}}   # unchanged")
    if new_onset != cur_onset:
        print(f"_TREE_EL_ONSET = {new_onset}   "
              f"# raised from {cur_onset}; shading evident at el_bin={max_qual_el}")
    else:
        print(f"_TREE_EL_ONSET = {cur_onset}   # unchanged")
    print()
    print("_TREE_SF_PROFILE = [")
    for el, sf in new_profile:
        if el in qualifying:
            n = qualifying[el][1]
            gap = qualifying[el][2]
            old_sf = orig_dict.get(el, sf)
            print(f"    ({el:2d}, {sf:.3f}),   "
                  f"# updated from {old_sf:.3f}  (n={n}, gap={gap:+.3f})")
        else:
            print(f"    ({el:2d}, {sf:.3f}),")
    print("]")


def main() -> None:
    """
    Load data, build calibration rows, update the CSV, and print summary.
    """
    print("Analysing tree shadow calibration (WNW zone)...")

    merged  = load_merged()
    new_rows = build_calibration_rows(merged)
    print(f"  {len(new_rows)} qualifying hourly observations in current dataset")

    combined = update_calibration_csv(new_rows)
    print(f"  {len(combined)} total rows in data/tree_shadow_calibration.csv")

    out_path = DATA_DIR / "tree_shadow_calibration.csv"
    print(f"  Updated: {out_path}")

    print_summary(combined)
    suggest_updates(combined)


if __name__ == "__main__":
    main()
