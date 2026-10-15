#!/usr/bin/env python3
"""
Fetch and accumulate 15-minute consumption data from Enlighten.

All four timeseries come from a single API call per day
(power_time_series?days=1&all_production_sources=true).

Fields extracted from the API response
---------------------------------------
consumption   data["consumption"] per slot.  Enphase computes this from all
              available meters (production, battery, grid) and reports the true
              home load directly.  This is NOT a raw CT reading — it correctly
              handles battery discharge to grid so no further correction is needed.

acb_c_ts      Battery charging power per slot (always >= 0, separate timeseries).
acb_d_ts      Battery discharge power per slot (always >= 0, separate timeseries).
              These are TWO separate fields, not a single signed value.

solar_w       production.meter per slot.

Derived quantities
------------------
grid_net_w    = consumption_w + charge_w - solar_w - discharge_w
                +ve = importing from grid, -ve = exporting to grid.

              Power balance: solar + discharge + grid_import
                           = home_load + charge + grid_export
              Rearranging: grid_net = home_load + charge - solar - discharge
              home_load = consumption_w (from Enphase), so:
              grid_net  = consumption_w + charge_w - solar_w - discharge_w

Calculation examples (verified against Enphase app and dumpdata CTs)
----------------------------------------------------------------------
All values in Watts.  grid_net +ve = import, -ve = export.

Case 1 — 16:00, battery discharging 6.2 kW to grid, solar 0.4 kW, home 0.2 kW:
  consumption=193  solar=408  charge=2  discharge=6195
  grid_net = 193 + 2 - 408 - 6195 = -6408W  (exporting 6.4 kW)
  Enphase app: Exported 6.4 kW, Consumed 0.2 kW  ✓
  Dumpdata CTs: power_w=-6091W, battery_w=+6745W  ✓

Case 2 — Night, battery discharging 0.67 kW to home, no grid:
  consumption=667  solar=0  charge=0  discharge=667
  grid_net = 667 + 0 - 0 - 667 = 0W  (grid idle)  ✓

Case 3 — Daytime, battery charging 4 kW from solar, exporting surplus:
  consumption=1000  solar=7000  charge=4000  discharge=0
  grid_net = 1000 + 4000 - 7000 - 0 = -2000W  (exporting 2 kW)  ✓

Australian DST days (added when migrated to BatterySolarMonitorUpdate,
2026-09-10): the API returns 92 raw slots on the October spring-forward day
(23h local day, 2am->3am never happens) and 100 raw slots on the April
fallback day (25h local day, 2am->3am happens twice). Both recur every year.
_remap_to_local_slots() maps either case onto the normal 96 local wall-clock
slots -- confirmed against real 2025-10-05 data this session by checking that
production ramp-up/down lines up with actual sunrise/sunset once the raw
index is shifted for the missing/repeated hour. Both DST hours are always
pre-dawn (2-3am), so this remap never touches a daylight slot the shading
model cares about; the previous behaviour (hard-reject anything != 96 slots)
would otherwise silently lose these two calendar days every single year.

_dst_transition_kind() gates the remap on the date ITSELF actually being the
first Sunday of October/April, not just on the slot count -- a non-96 count
on any other date is a genuine data anomaly (e.g. a brief monitoring outage),
not a DST shift, and remapping it the same way would silently corrupt real
data. Caught for real during this same migration's smoke test:
2026-07-06 (an ordinary Monday) came back with 92 slots from what was
evidently an outage, not daylight saving.

97-slot days (found 2026-09-27: every date queried from ~mid-2026 onward,
including long-settled historical ones re-fetched fresh, now comes back
with 97 raw slots instead of 96 -- an Enlighten-side API change, not a
per-day anomaly). Confirmed via a live fetch of 2026-09-08's raw
production.meter: sunrise/sunset (06:15/17:15, indices 25/69) land exactly
where a normal 96-slot day puts them, and index 96 is 24:00 -- the very
start of the NEXT day -- with a small/zero value consistent with an
overnight instant, not a real 97th interval of the requested day. Index 0
of the following day's own fetch does NOT equal this index-96 value, so
it's a boundary POINT sample (this day's data ends at 24:00:00), not a
copy of tomorrow's first 15-min interval average. Dropped by truncating to
the first 96 slots before the usual DST-length checks -- without this,
every day since the API change would hit the "unexpected length" branch
and be silently skipped, exactly like 2026-07-06 above.

Behaviour
---------
  - Only fetches complete past days (never today).
  - First run: fetches the last 7 complete days.
  - Subsequent runs: any days missing since earliest stored date up to yesterday.
  - Each day written once and kept permanently.

Outputs
-------
  data/consumption_15min_history.csv      corrected home consumption (kW)
  data/solar_15min_history.csv            solar production (kW)
  data/battery_charge_15min_history.csv   battery charging power (kW, always >= 0)
  data/grid_15min_history.csv             grid net (kW, +ve import, -ve export)
"""

from __future__ import annotations

import json
import sys
import urllib.request
from datetime import date, timedelta
from pathlib import Path

import pandas as pd

PROJECT_DIR    = Path(__file__).parent          # py_programs/
ROOT_DIR       = PROJECT_DIR.parent             # BatterySolarMonitorUpdate/
DATA_DIR       = ROOT_DIR / "data"
OUT_PATH       = DATA_DIR / "consumption_15min_history.csv"
SOLAR_OUT_PATH = DATA_DIR / "solar_15min_history.csv"
BATT_OUT_PATH  = DATA_DIR / "battery_charge_15min_history.csv"
GRID_OUT_PATH  = DATA_DIR / "grid_15min_history.csv"
# Both one level above BatterySolarMonitorUpdate/ (i.e. ../ from ROOT_DIR)
# -- CONFIG_FILE is site_settings.csv, this whole pipeline's identity
# config (human-managed, required; see auth/enphase_token.py's own module
# docstring for why it's this file and not a separate one); SESSION_FILE
# is regenerated fresh by auth/enphase_token.py every run and deleted by
# quarterly_update.py once the whole run finishes.
CONFIG_FILE    = ROOT_DIR.parent / "site_settings.csv"
SESSION_FILE   = ROOT_DIR.parent / "_session.csv"


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

SLOTS = 96   # 15-min intervals in a normal 24h day
SLOT_COLS = [f"slot_{i}" for i in range(SLOTS)]

# The 2am->3am jump/repeat always falls in slots 8-11 (02:00-02:59).
DST_JUMP_RAW_INDEX = 8


def enlighten_headers(session: str) -> dict:
    """Return authentication headers for the Enlighten web API."""
    return {
        "Cookie": f"_enlighten_4_session={session}",
        "e-auth-token": session,
        "Accept": "application/json",
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
        "X-Requested-With": "XMLHttpRequest",
    }


def _dst_transition_kind(d: date) -> str | None:
    """
    Return "spring" if d is the AEDT spring-forward date (first Sunday of
    October), "fallback" if d is the AEDT fallback date (first Sunday of
    April), else None.

    A non-96 slot count is NOT on its own evidence of a DST day -- a real
    data gap (e.g. a brief Enphase/gateway outage) can also produce a
    short response on an ordinary date. Confirmed this session (2026-09-10
    quarterly_update.py smoke test): 2026-07-06 came back with 92 slots
    despite being nowhere near either transition date. Remapping that day
    as if it were DST would have silently shifted real data by up to 4
    slots. Only apply the remap when the calendar date itself is the actual
    transition date; any other non-96 length is treated as a genuine
    anomaly (skipped), same as the original hard rejection.
    """
    if d.month not in (4, 10):
        return None
    first_of_month = date(d.year, d.month, 1)
    first_sunday = first_of_month + timedelta(days=(6 - first_of_month.weekday()) % 7)
    if d != first_sunday:
        return None
    return "spring" if d.month == 10 else "fallback"


def _remap_to_local_slots(raw: list[float]) -> list[float] | None:
    """
    Map a raw (possibly DST-affected) slot array onto the normal 96 local
    wall-clock slots. See the module docstring for the reasoning.

    n == 96 (normal day): returned unchanged.
    n == 92 (October spring-forward, 23h day): raw index i < 8 maps to local
        slot i unchanged; from the jump onward local slot = i + 4. Local
        slots 8-11 (02:00-02:59, which never occurred) are left at 0.0.
    n == 100 (April fallback, 25h day): raw indices 0-7 unchanged; the two
        occurrences of 02:00-02:59 (raw 8-11 and 12-15) are averaged into
        local slots 8-11; raw indices 16+ map to local index - 4.
    Any other length: returns None (caller treats as an error, same as the
    original hard rejection).
    """
    n = len(raw)
    if n == SLOTS:
        return list(raw)
    if n == SLOTS - 4:
        out = [0.0] * SLOTS
        for i, v in enumerate(raw):
            local = i if i < DST_JUMP_RAW_INDEX else i + 4
            out[local] = v
        return out
    if n == SLOTS + 4:
        out = [0.0] * SLOTS
        for i in range(DST_JUMP_RAW_INDEX):
            out[i] = raw[i]
        for i in range(DST_JUMP_RAW_INDEX, DST_JUMP_RAW_INDEX + 4):
            out[i] = (raw[i] + raw[i + 4]) / 2.0
        for i in range(DST_JUMP_RAW_INDEX + 8, n):
            out[i - 4] = raw[i]
        return out
    return None


def fetch_day(d: date, session: str, system_id: str) -> dict | None:
    """
    Fetch 15-min timeseries for one complete past day from Enlighten.

    Returns a dict with keys:
        "consumption"     : 96-element list of watts (corrected home load)
        "production"      : 96-element list of watts (solar meter)
        "battery_charge"  : 96-element list of watts (charging power, >= 0)
        "grid_net"        : 96-element list of watts (+ve import, -ve export)
    Returns None if data is missing or the slot count isn't a recognised
    (normal or DST) length.
    """
    url = (
        f"https://enlighten.enphaseenergy.com/systems/{system_id}"
        f"/power_time_series?all_production_sources=true&days=1&date={d}"
    )
    req = urllib.request.Request(url, headers=enlighten_headers(session))
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            data = json.load(r)
    except Exception as e:
        print(f"  {d}: fetch failed ({e})")
        return None

    # Consumption: Enphase computes the true home load and provides it directly.
    consumption_raw = data.get("consumption", [])
    n = len(consumption_raw)
    if n == SLOTS + 1:
        # Trailing day-boundary point sample (24:00, i.e. the next day's own
        # 00:00 instant) -- see module docstring. _pad()'s range(SLOTS) bound
        # below naturally drops this same trailing element from every raw
        # array (consumption/acb_c_ts/acb_d_ts/production.meter) once n is
        # normalised to SLOTS here.
        print(f"  {d}: {n} raw slots -- dropping trailing day-boundary sample (24:00)")
        n = SLOTS
    dst_kind = _dst_transition_kind(d)
    if n == SLOTS:
        pass
    elif n == SLOTS - 4 and dst_kind == "spring":
        print(f"  {d}: DST spring-forward (23h) day ({n} raw slots) -- remapping to {SLOTS} local slots")
    elif n == SLOTS + 4 and dst_kind == "fallback":
        print(f"  {d}: DST fallback (25h) day ({n} raw slots) -- remapping to {SLOTS} local slots")
    else:
        reason = " (not the actual DST transition date -- treating as a data gap)" if n in (SLOTS - 4, SLOTS + 4) else ""
        print(f"  {d}: consumption timeseries unexpected length ({n}){reason} -- skipping")
        return None

    # Battery charge and discharge are separate timeseries (both always >= 0).
    acb          = data.get("acb", {})
    acb_c_raw    = acb.get("acb_c_ts", [])   # charging power
    acb_d_raw    = acb.get("acb_d_ts", [])   # discharge power
    prod_raw     = data.get("production", {}).get("meter", [])

    def _pad(raw: list, length: int) -> list[float]:
        return [float(raw[i]) if i < len(raw) else 0.0 for i in range(length)]

    consumption_watts    = _remap_to_local_slots(_pad(consumption_raw, n))
    battery_charge_watts = _remap_to_local_slots(_pad(acb_c_raw, n))
    battery_discharge_w  = _remap_to_local_slots(_pad(acb_d_raw, n))
    production_watts     = _remap_to_local_slots(_pad(prod_raw, n))

    grid_net_watts = []
    for i in range(SLOTS):
        # Grid net from power balance:
        #   solar + discharge + grid_import = home_load + charge + grid_export
        #   grid_net = home_load + charge - solar - discharge
        # +ve = importing from grid, -ve = exporting to grid.
        grid_net_w = (consumption_watts[i] + battery_charge_watts[i]
                      - production_watts[i] - battery_discharge_w[i])
        grid_net_watts.append(grid_net_w)

    return {
        "consumption":    consumption_watts,
        "production":     production_watts,
        "battery_charge": battery_charge_watts,
        "grid_net":       grid_net_watts,
    }


def day_to_row(d: date, watts: list[float]) -> dict:
    """
    Convert a day's 96-element watt list to a wide-format dict.
    Values are converted from watts to kW, rounded to 4 decimal places.
    """
    row = {"date": str(d)}
    for i, w in enumerate(watts):
        row[f"slot_{i}"] = round(w / 1000, 4)
    return row


def _merge_and_save(new_rows: list[dict], existing_path: Path) -> None:
    """Merge new rows into an existing wide CSV (or create it) and save."""
    new_df = pd.DataFrame(new_rows)
    if existing_path.exists():
        existing = pd.read_csv(existing_path, dtype={"date": str})
        if "slot_0" in existing.columns:
            new_df = pd.concat([existing, new_df], ignore_index=True)
    new_df = (new_df
              .drop_duplicates(subset="date")
              .sort_values("date")
              .reset_index(drop=True))
    new_df.to_csv(existing_path, index=False)
    print(f"Saved {len(new_df)} days to {existing_path.name}")


def main() -> None:
    import warnings as _warnings
    _warn_log = ROOT_DIR / "docs" / "charts" / "quarterly_update.log"
    def _warn_to_log(message, category, filename, lineno, file=None, line=None):
        with open(_warn_log, "a", encoding="ascii") as _lf:
            _lf.write(f"WARNING [{category.__name__}]: {message}  ({filename}:{lineno})\n")
    _warnings.showwarning = _warn_to_log

    if not CONFIG_FILE.exists():
        print(f"ERROR: {CONFIG_FILE} not found -- run auth/enphase_token.py first "
              f"(see {CONFIG_FILE.name}.sample in auth/ for the required format).")
        sys.exit(1)

    config    = _load_kv_csv(CONFIG_FILE)
    system_id = config.get("enlighten_system_id", "")

    session = _load_kv_csv(SESSION_FILE).get("enlighten_session", "")
    if not session:
        # No session yet -- expected/self-healing (auth/enphase_token.py's
        # own next successful run will create SESSION_FILE), NOT the same
        # as CONFIG_FILE being missing entirely (checked above).
        print("No enlighten_session in session file.")
        return

    today     = date.today()
    yesterday = today - timedelta(days=1)

    # ── Load existing consumption dates ────────────────────────────────────
    existing_cons = None
    fetched_dates: set[str] = set()
    if OUT_PATH.exists():
        df = pd.read_csv(OUT_PATH, dtype={"date": str})
        if "slot_0" in df.columns:
            existing_cons = df
            fetched_dates = set(df["date"].unique())
            print(f"Loaded {len(fetched_dates)} stored consumption days  "
                  f"({min(fetched_dates)} to {max(fetched_dates)})")
        else:
            print("Existing consumption file has old format -- will regenerate.")

    # ── Decide which days to fetch ─────────────────────────────────────────
    if fetched_dates:
        earliest = date.fromisoformat(min(fetched_dates))
    else:
        earliest = yesterday - timedelta(days=6)   # first run: last 7 complete days

    to_fetch = [
        d for d in (earliest + timedelta(days=i)
                    for i in range((yesterday - earliest).days + 1))
        if str(d) not in fetched_dates
    ]

    # Days where consumption is stored but solar (or battery/grid) may be missing
    solar_dates: set[str] = set()
    if SOLAR_OUT_PATH.exists():
        sf = pd.read_csv(SOLAR_OUT_PATH, dtype={"date": str})
        if "slot_0" in sf.columns:
            solar_dates = set(sf["date"].unique())
    solar_only = [
        d for d in (earliest + timedelta(days=i)
                    for i in range((yesterday - earliest).days + 1))
        if str(d) not in solar_dates and str(d) not in [str(x) for x in to_fetch]
    ]
    to_fetch_solar_only = solar_only
    to_fetch_all = to_fetch + to_fetch_solar_only

    if not to_fetch_all:
        print("All days up to yesterday are already stored.")
        return

    print(f"Fetching {len(to_fetch)} new day(s) + {len(to_fetch_solar_only)} solar-only day(s) ...")

    # ── Fetch ──────────────────────────────────────────────────────────────
    new_cons_rows   = []
    new_solar_rows  = []
    new_batt_rows   = []
    new_grid_rows   = []

    for d in to_fetch_all:
        result = fetch_day(d, session, system_id)
        if result is None:
            continue
        new_solar_rows.append(day_to_row(d, result["production"]))
        new_batt_rows.append(day_to_row(d, result["battery_charge"]))
        new_grid_rows.append(day_to_row(d, result["grid_net"]))
        if d not in to_fetch_solar_only:
            new_cons_rows.append(day_to_row(d, result["consumption"]))
        cons_mean  = sum(result["consumption"])   / SLOTS / 1000
        solar_mean = sum(result["production"])    / SLOTS / 1000
        grid_mean  = sum(result["grid_net"])      / SLOTS / 1000
        batt_mean  = sum(result["battery_charge"])/ SLOTS / 1000
        print(f"  {d}  cons={cons_mean:.2f} kW  solar={solar_mean:.2f} kW"
              f"  grid={grid_mean:+.2f} kW  batt_chg={batt_mean:.2f} kW")

    if not new_cons_rows and not new_solar_rows:
        print("Nothing fetched -- no changes saved.")
        return

    # ── Save all four CSVs ─────────────────────────────────────────────────
    if new_cons_rows:
        _merge_and_save(new_cons_rows, OUT_PATH)
    if new_solar_rows:
        _merge_and_save(new_solar_rows, SOLAR_OUT_PATH)
    if new_batt_rows:
        _merge_and_save(new_batt_rows, BATT_OUT_PATH)
    if new_grid_rows:
        _merge_and_save(new_grid_rows, GRID_OUT_PATH)


if __name__ == "__main__":
    import traceback as _tb
    try:
        main()
    except Exception:
        print(f"UNHANDLED ERROR in {Path(__file__).name}:")
        print(_tb.format_exc())
        raise SystemExit(1)
