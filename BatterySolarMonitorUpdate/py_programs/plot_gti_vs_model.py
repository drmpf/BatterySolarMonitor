#!/usr/bin/env python3
"""
Compare raw Open-Meteo GTI forecast (converted to kW) against the
shading-corrected solar model output and actual production.

Reads:
  data/calibrated_solar_hourly_v2.csv   -- gti_wm2, shade_factor, est_kw_shading
  data/solar_15min_history.csv          -- 96 15-min slots/day, averaged to
                                            hourly actual_kw here

Produces:
  docs/charts/gti_vs_model.png

Usage:
  python plot_gti_vs_model.py [--days N] [--start YYYY-MM-DD] [--end YYYY-MM-DD]

Defaults to the last 7 days ending on the most recent date actually
present in data/solar_15min_history.csv (not "today - 1"), so the chart
still shows real days if the latest fetch fell short of yesterday.

Migrated to BatterySolarMonitorUpdate 2026-09-10: the original
(dailyPrediction/plot_gti_vs_model.py) named
data/solar_hourly_forecast_vs_actual.csv (written by
dailyPrediction/plot_today_solar.py, not migrated) as its actual-production
source, and its own docstring mentioned a 'forecast_kw' column -- but the
code never actually reads forecast_kw, only actual_kw. Same situation as
analyse_tree_shadow.py: load_data() below now builds that (date, hour,
actual_kw) shape from solar_15min_history.csv instead, the source every
other script in this pipeline already uses. No capability lost.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from datetime import date, timedelta

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.dates as mdates
import numpy as np
import pandas as pd

PROJECT_DIR = Path(__file__).parent          # py_programs/
ROOT_DIR    = PROJECT_DIR.parent             # BatterySolarMonitorUpdate/
DATA_DIR    = ROOT_DIR / "data"
CHART_DIR   = ROOT_DIR / "docs" / "charts"

sys.path.insert(0, str(PROJECT_DIR))
from solar_shading_model import SYSTEM_KW  # noqa: E402 -- nameplate capacity, see that module's own docstring


def latest_available_date() -> date | None:
    """
    Most recent date present in solar_15min_history.csv, or None if the
    file is missing/empty. Used as the default chart end-date instead of
    "today - 1" so a run right after a fetch that fell short of yesterday
    (a skipped day, a partial pipeline failure) still shows real days
    instead of a trailing "No data" panel.
    """
    path = DATA_DIR / "solar_15min_history.csv"
    if not path.exists():
        return None
    sh = pd.read_csv(path, usecols=["date"], dtype={"date": str})
    if sh.empty:
        return None
    return date.fromisoformat(sh["date"].max())


def _hourly_actual_from_15min() -> pd.DataFrame:
    """
    Build a (date, hour, actual_kw) table from solar_15min_history.csv --
    same mean-of-4-slots transform used throughout this pipeline (see
    analyse_tree_shadow.py's identically-named helper).
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


def load_data(start_date: date, end_date: date) -> pd.DataFrame:
    """
    Load and merge hourly GTI/model data with actual production for the
    given inclusive date range.

    Parameters:
        start_date  -- first date to include
        end_date    -- last date to include (inclusive)

    Returns a DataFrame with columns:
        timestamp   -- hourly datetime
        gti_wm2     -- raw Open-Meteo GTI (W/m²)
        shade_factor
        base_kw     -- SYSTEM_KW * gti_wm2 / 1000  (unshaded forecast kW)
        model_kw    -- shading-corrected model output (kW)
        actual_kw   -- measured production (kW), NaN where unavailable
    """
    # --- calibrated hourly CSV: GTI + model ----------------------------------
    calib_path = DATA_DIR / "calibrated_solar_hourly_v2.csv"
    calib = pd.read_csv(
        calib_path,
        usecols=["timestamp", "gti_wm2", "shade_factor", "est_kw_shading"],
    )
    calib["timestamp"] = pd.to_datetime(calib["timestamp"])
    calib = calib.rename(columns={"est_kw_shading": "model_kw"})
    calib["base_kw"] = (SYSTEM_KW * calib["gti_wm2"] / 1000.0).clip(0, SYSTEM_KW)

    start_ts = pd.Timestamp(start_date)
    end_ts   = pd.Timestamp(end_date) + pd.Timedelta(hours=23)
    calib = calib[(calib["timestamp"] >= start_ts) & (calib["timestamp"] <= end_ts)].copy()

    # --- actual production: derived from solar_15min_history.csv -------------
    actual = _hourly_actual_from_15min()
    actual["timestamp"] = pd.to_datetime(actual["date"]) + pd.to_timedelta(actual["hour"], unit="h")
    actual = actual[["timestamp", "actual_kw"]]

    # --- merge ---------------------------------------------------------------
    merged = pd.merge(calib, actual, on="timestamp", how="left")
    return merged.sort_values("timestamp").reset_index(drop=True)


def plot(df: pd.DataFrame, start_date: date, end_date: date) -> None:
    """
    Plot raw GTI forecast kW, shading-corrected model kW, and actual kW
    on a single chart.  Daytime hours only (gti_wm2 > 0 or actual > 0).

    Parameters:
        df          -- merged DataFrame from load_data()
        start_date  -- first date (used in chart title)
        end_date    -- last date (used in chart title)
    """
    # Only plot daytime rows to avoid a flat zero baseline cluttering the chart
    day_mask = (df["gti_wm2"] > 0) | (df["actual_kw"].fillna(0) > 0)
    df_day = df[day_mask].copy()

    n_days = (end_date - start_date).days + 1

    fig, axes = plt.subplots(
        n_days, 1,
        figsize=(14, 3 * n_days),
        squeeze=False,
        sharex=False,
    )
    fig.patch.set_facecolor("#F5F5F5")

    C_GTI   = "#F9A825"   # amber  -- raw Open-Meteo GTI
    C_MODEL = "#4CAF50"   # green  -- shading-corrected model
    C_ACT   = "#1B7A2E"   # dark green -- actual
    C_LIGHT = "#CCCCCC"
    C_TEXT  = "#333333"

    for i, current_date in enumerate(
        start_date + timedelta(days=d) for d in range(n_days)
    ):
        ax = axes[i][0]
        ax.set_facecolor("#FAFAFA")
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)
        ax.spines["left"].set_color(C_LIGHT)
        ax.spines["bottom"].set_color(C_LIGHT)
        ax.tick_params(colors=C_TEXT, labelsize=8)

        day_df = df[df["timestamp"].dt.date == current_date]

        if day_df.empty:
            ax.text(0.5, 0.5, "No data", transform=ax.transAxes,
                    ha="center", va="center", color=C_TEXT)
            ax.set_title(str(current_date), fontsize=9, fontweight="bold", color=C_TEXT)
            continue

        ts       = day_df["timestamp"]
        base_kw  = day_df["base_kw"]
        model_kw = day_df["model_kw"]
        act_kw   = day_df["actual_kw"]

        # Raw GTI forecast (unshaded)
        ax.fill_between(ts, base_kw, alpha=0.18, color=C_GTI)
        ax.plot(ts, base_kw,
                color=C_GTI, linewidth=1.5, linestyle="--",
                label="Open-Meteo GTI (unshaded)")

        # Shading-corrected model
        ax.fill_between(ts, model_kw, alpha=0.18, color=C_MODEL)
        ax.plot(ts, model_kw,
                color=C_MODEL, linewidth=1.5,
                label="Shading-corrected model")

        # Actual production
        act_valid = act_kw.notna()
        if act_valid.any():
            ax.plot(ts[act_valid], act_kw[act_valid],
                    color=C_ACT, linewidth=2, linestyle="-",
                    label="Actual production")

        # Day totals in title
        base_kwh  = float(base_kw.sum())
        model_kwh = float(model_kw.sum())
        act_kwh   = float(act_kw.sum()) if act_valid.any() else float("nan")
        act_str   = f"  actual={act_kwh:.1f}" if not np.isnan(act_kwh) else ""
        ax.set_title(
            f"{current_date.strftime('%A %d %b %Y')}  |  "
            f"GTI={base_kwh:.1f} kWh  model={model_kwh:.1f} kWh{act_str}",
            fontsize=9, fontweight="bold", color=C_TEXT, loc="left",
        )

        ax.axhline(SYSTEM_KW, color=C_LIGHT, linewidth=0.8, linestyle=":",
                   label=f"Nameplate ({SYSTEM_KW} kW)")

        ax.set_ylim(bottom=0)
        ax.set_ylabel("kW", fontsize=8, color=C_TEXT)
        ax.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
        ax.xaxis.set_major_locator(mdates.HourLocator(interval=2))
        plt.setp(ax.xaxis.get_majorticklabels(), rotation=45, ha="right",
                 fontsize=7)

        if i == 0:
            ax.legend(fontsize=8, frameon=False, loc="upper left")

    date_range_str = (
        str(start_date) if start_date == end_date
        else f"{start_date} to {end_date}"
    )
    fig.suptitle(
        f"Open-Meteo GTI forecast vs Shading Model  —  {date_range_str}",
        fontsize=11, fontweight="bold", color=C_TEXT, y=1.002,
    )
    fig.tight_layout()

    CHART_DIR.mkdir(parents=True, exist_ok=True)
    out_path = CHART_DIR / "gti_vs_model.png"
    fig.savefig(out_path, dpi=120, bbox_inches="tight",
                facecolor=fig.get_facecolor())
    plt.close(fig)
    print(f"  Saved: {out_path}")


def main() -> None:
    """
    Parse command-line arguments and run the GTI vs model comparison plot.

    Arguments:
        --days N             -- plot the last N complete days (default 7)
        --start YYYY-MM-DD   -- explicit start date (overrides --days)
        --end   YYYY-MM-DD   -- explicit end date   (overrides --days)
    """
    parser = argparse.ArgumentParser(
        description="Plot Open-Meteo GTI forecast vs shading model vs actual"
    )
    parser.add_argument("--days",  type=int, default=7,
                        help="Number of recent days to plot (default 7)")
    parser.add_argument("--start", type=str, default=None,
                        help="Start date YYYY-MM-DD")
    parser.add_argument("--end",   type=str, default=None,
                        help="End date YYYY-MM-DD (inclusive)")
    args = parser.parse_args()

    today = date.today()

    if args.start:
        start_date = date.fromisoformat(args.start)
        end_date   = date.fromisoformat(args.end) if args.end else today - timedelta(days=1)
    else:
        if args.end:
            end_date = date.fromisoformat(args.end)
        else:
            end_date = latest_available_date() or (today - timedelta(days=1))
        start_date = end_date - timedelta(days=args.days - 1)

    print(f"Plotting GTI vs model: {start_date} to {end_date}")
    df = load_data(start_date, end_date)
    if df.empty:
        print("  No data found for the requested date range.")
        return

    print(f"  {len(df)} hourly rows loaded")
    plot(df, start_date, end_date)


if __name__ == "__main__":
    main()
