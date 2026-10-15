#!/usr/bin/env python3
"""
Incrementally fetch hourly solar irradiance history from Open-Meteo and
append missing dates to data/solar_history.csv.

On first run (no existing file) fetches from FETCH_START to yesterday.
On subsequent runs fetches only from (last stored date + 1) to yesterday.

Panel configuration: LAT/LON/PANEL_TILT/PANEL_AZIMUTH below are read from
../site_settings.csv's latitude/longitude/panel_tilt/panel_azimuth keys
(one level above BatterySolarMonitorUpdate/, this whole pipeline's own
entirely human-maintained config file -- see auth/enphase_token.py's own
module docstring). This is THE Python-side source for these 4 values --
solar_shading_model.py and every diag_*.py/plot_gti_vs_model.py script
that needs them imports from here rather than each keeping its own
separate copy (2026-09-27: they used to, and had drifted into several
independently-hardcoded copies of the same physical facts).
"""

from __future__ import annotations

import json
import sys
import time
import urllib.request
import urllib.parse
from datetime import date, timedelta, timezone as dt_timezone
from pathlib import Path

import pandas as pd
from dateutil import tz as dateutil_tz

PROJECT_DIR = Path(__file__).parent          # py_programs/
ROOT_DIR    = PROJECT_DIR.parent             # BatterySolarMonitorUpdate/
DATA_DIR    = ROOT_DIR / "data"
OUT_PATH    = DATA_DIR / "solar_history.csv"

# One level above BatterySolarMonitorUpdate/ -- see module docstring.
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
_REQUIRED = ("latitude", "longitude", "panel_tilt", "panel_azimuth", "posix_timezone")
_missing = [k for k in _REQUIRED if not _config.get(k)]
if _missing:
    print(f"ERROR: {CONFIG_FILE} is missing a value for {_missing} "
          f"(see auth/site_settings.csv.sample).")
    sys.exit(1)

LAT                 = float(_config["latitude"])
LON                 = float(_config["longitude"])
PANEL_TILT          = float(_config["panel_tilt"])
PANEL_AZIMUTH       = float(_config["panel_azimuth"])   # 0=South, -90=East, 90=West
# POSIX TZ string -- THE timezone source for main()'s own history fetch
# (fetch_chunk_utc()), converted via dateutil.tz.tzstr() (see that
# function's own docstring for why). open_meteo_timezone (an IANA name)
# was removed entirely 2026-09-28 -- Open-Meteo's archive API was found to
# apply a single FIXED, non-DST-aware UTC offset for an entire IANA-timezone
# request, silently mislabeling ~half of every year's timestamps by 1h
# during daylight saving; posix_timezone + dateutil.tz.tzstr() is correct
# where that was not. diag_tomorrow.py's own live forecast call was
# switched to the same approach (see that script).
POSIX_TIMEZONE = _config["posix_timezone"]

FETCH_START  = date(2023, 1, 1)
ARCHIVE_URL  = "https://archive-api.open-meteo.com/v1/archive"

HOURLY_VARS = [
    "global_tilted_irradiance",
    "cloud_cover",
    "direct_radiation",
    "diffuse_radiation",
]


def utc_naive_to_local(utc_naive: pd.Series, posix_tz: str) -> pd.Series:
    """
    Convert a Series of timezone-naive UTC timestamps (e.g. straight from
    an Open-Meteo response fetched with timezone=GMT+0) to timezone-naive
    LOCAL timestamps, using posix_tz (a POSIX TZ string, e.g.
    site_settings.csv's posix_timezone value) via dateutil.tz.tzstr().

    Shared by fetch_chunk_utc() (this module) and diag_tomorrow.py's own
    live forecast fetch -- both switched from an IANA-name timezone=
    parameter to GMT+0 + this conversion on 2026-09-28 (see
    fetch_chunk_utc()'s own docstring for why: Open-Meteo's own IANA-name
    handling applies a single fixed, non-DST-aware UTC offset per request).

    IMPORTANT: converts each timestamp via plain stdlib datetime.astimezone()
    (through .to_pydatetime() first), NEVER pandas' vectorized
    .dt.tz_convert() or pandas.Timestamp.astimezone() directly -- both go
    through pandas' internal fast path (get_dst_info), which assumes a
    pytz/zoneinfo-style tzinfo with precomputed transition tables.
    Confirmed 2026-09-28: on a dateutil.tz.tzstr() object this doesn't
    raise cleanly, it fails INSIDE a destructor (printed as a non-fatal
    "Exception ignored in..." traceback) and pandas.Timestamp.astimezone()
    then silently returns the ORIGINAL, UNCONVERTED value instead of
    raising -- a real, silent wrong-data bug, not a crash. Plain stdlib
    datetime.astimezone() uses tzstr's own utcoffset()/dst()/fromutc()
    methods directly and is confirmed correct.
    """
    utc_ts   = pd.to_datetime(utc_naive).dt.tz_localize(dt_timezone.utc)
    local_tz = dateutil_tz.tzstr(posix_tz)
    return utc_ts.apply(lambda t: t.to_pydatetime().astimezone(local_tz).replace(tzinfo=None))


def fetch_chunk_utc(start: date, end: date, posix_tz: str) -> pd.DataFrame:
    """
    Fetch one LOCAL date range from the Open-Meteo archive API. Requests
    Open-Meteo with timezone=GMT+0 (not an IANA name), then converts each
    returned UTC hourly timestamp to local wall-clock time using posix_tz
    (site_settings.csv's posix_timezone value, e.g.
    "AEST-10AEDT,M10.1.0,M4.1.0/3") via dateutil.tz.tzstr().

    Replaced the original fetch_chunk() (2026-09-28), which requested
    Open-Meteo with an IANA timezone name directly and trusted its
    'local-time' output. Found via a live side-by-side comparison
    (diag_compare_om_timezone_methods.py, now removed -- its job is done)
    that Open-Meteo's archive API applies a single FIXED, non-DST-aware
    UTC offset to an ENTIRE request when given an IANA name -- confirmed
    live: requesting just January 2025 (Sydney, genuinely AEDT/+11 then)
    still returned utc_offset_seconds=36000 (AEST/+10, wrong), identical
    to a July-2025-only request (correctly AEST/+10) and a full-year
    request. This silently mislabeled roughly half of every year's stored
    timestamps by 1h -- every daylight-saving period since this pipeline
    began (2023) -- which solar_shading_model.py then used to compute sun
    elevation/azimuth for binning against Enphase's own (correctly
    timestamped) actual production, misattributing GTI/production data to
    the wrong sun-position bin for that whole 1h-shifted stretch. This
    function's own conversion is DST-correct (verified against the real
    posix_timezone value across a winter/summer date pair and exactly at
    the true 2026-10-04 02:00 spring-forward instant).

    See utc_naive_to_local()'s own docstring for why dateutil.tz.tzstr() is
    the right conversion tool here (not zoneinfo, not the ESP32's own
    time.tzset() mechanism) and the pandas/dateutil pitfall to avoid.

    Buffering: Open-Meteo aligns start_date/end_date to whichever
    timezone is requested (confirmed live 2026-09-28), so requesting
    timezone=GMT+0 means the returned data's own day boundaries are UTC
    days, not local ones. A local calendar day can be pulled into the
    adjacent UTC day by at most one full day, in one direction or the
    other depending on the site's UTC offset SIGN (a positive offset --
    like this site's -- pulls the START of a local day back into the
    previous UTC day; a negative offset pushes the END of a local day
    forward into the next UTC day). Since this is shared, site-agnostic
    code, one buffer UTC day is fetched on EACH side and trimmed after
    conversion, covering either sign without needing to know it in
    advance -- this is also exactly how a missing local day at the very
    start/end of a fetched year is avoided (the point of a 1-day-only
    buffer on each side, not a larger one: |UTC offset| is always < 24h,
    so one full UTC day of buffer is always enough to fully cover one
    calendar-day boundary in either direction).

    Parameters
    ----------
    start, end : date -- first/last LOCAL day to fetch (inclusive).
    posix_tz : str -- POSIX TZ string, e.g. site_settings.csv's
        posix_timezone value.

    Returns
    -------
    pd.DataFrame with a 'timestamp' column (local, timezone-naive) plus
    all HOURLY_VARS columns, trimmed to exactly the requested local
    [start, end] range.
    """
    utc_start = start - timedelta(days=1)
    utc_end   = end + timedelta(days=1)
    params = {
        "latitude":   LAT,
        "longitude":  LON,
        "start_date": utc_start.isoformat(),
        "end_date":   utc_end.isoformat(),
        "hourly":     ",".join(HOURLY_VARS),
        "tilt":       PANEL_TILT,
        "azimuth":    PANEL_AZIMUTH,
        "timezone":   "GMT+0",
    }
    url = ARCHIVE_URL + "?" + urllib.parse.urlencode(params)
    print(f"  Fetching {utc_start} to {utc_end} (UTC, buffered) ...", end="", flush=True)
    with urllib.request.urlopen(url, timeout=60) as resp:
        payload = json.loads(resp.read())
    df = pd.DataFrame(payload["hourly"])
    df.rename(columns={"time": "timestamp"}, inplace=True)
    df["timestamp"] = utc_naive_to_local(df["timestamp"], posix_tz)

    mask = (df["timestamp"].dt.date >= start) & (df["timestamp"].dt.date <= end)
    df = df[mask].reset_index(drop=True)
    print(f"  {len(df)} rows (trimmed to local {start}..{end})")
    return df


def main() -> None:
    import warnings as _warnings
    _warn_log = ROOT_DIR / "docs" / "charts" / "quarterly_update.log"
    def _warn_to_log(message, category, filename, lineno, file=None, line=None):
        with open(_warn_log, "a", encoding="ascii") as _lf:
            _lf.write(f"WARNING [{category.__name__}]: {message}  ({filename}:{lineno})\n")
    _warnings.showwarning = _warn_to_log

    DATA_DIR.mkdir(exist_ok=True)
    yesterday = date.today() - timedelta(days=1)

    # ── Load existing data ───────────────────────────────────────────────────
    existing   = None
    fetch_from = FETCH_START

    if OUT_PATH.exists():
        existing = pd.read_csv(OUT_PATH)
        existing["timestamp"] = pd.to_datetime(existing["timestamp"])
        last_stored = existing["timestamp"].dt.date.max()
        fetch_from  = last_stored + timedelta(days=1)
        print(f"Existing solar_history.csv: last date {last_stored}")

    if fetch_from > yesterday:
        print("solar_history.csv is already up to date.")
        return

    print(f"Fetching {fetch_from} to {yesterday} ...")

    # ── Fetch year-by-year to stay within API limits ─────────────────────────
    chunks: list[pd.DataFrame] = []
    chunk_start = fetch_from

    while chunk_start <= yesterday:
        chunk_end = min(date(chunk_start.year, 12, 31), yesterday)
        df = fetch_chunk_utc(chunk_start, chunk_end, POSIX_TIMEZONE)
        chunks.append(df)
        chunk_start = date(chunk_start.year + 1, 1, 1)
        if chunk_start <= yesterday:
            time.sleep(0.5)

    new_df = pd.concat(chunks, ignore_index=True)

    # ── Merge with existing ──────────────────────────────────────────────────
    if existing is not None:
        combined = pd.concat([existing, new_df], ignore_index=True)
        combined.drop_duplicates(subset="timestamp", inplace=True)
    else:
        combined = new_df

    combined.sort_values("timestamp", inplace=True)
    combined.reset_index(drop=True, inplace=True)
    combined.to_csv(OUT_PATH, index=False, float_format="%.2f")

    n_days = combined["timestamp"].dt.date.nunique()
    print(f"Saved {len(combined):,} rows ({n_days} days) to {OUT_PATH}")
    print(f"  From : {combined['timestamp'].iloc[0]}")
    print(f"  To   : {combined['timestamp'].iloc[-1]}")


if __name__ == "__main__":
    import traceback as _tb
    try:
        main()
    except Exception:
        print(f"UNHANDLED ERROR in {Path(__file__).name}:")
        print(_tb.format_exc())
        raise SystemExit(1)
