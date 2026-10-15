#!/usr/bin/env python3
"""
Incrementally fetch daily solar production and consumption from Enlighten
and append missing dates to data/enphase_daily.csv.

Uses enlighten_system_id from ../site_settings.csv (this whole pipeline's
entirely human-maintained identity/site config, one level above
BatterySolarMonitorUpdate/ -- see auth/enphase_token.py's own module
docstring) and the _enlighten_4_session cookie from ../_session.csv
(regenerated every run -- see that same docstring). If no session is
available yet, the script exits without modifying the output file so
existing data is preserved. Run auth/enphase_token.py first
(quarterly_update.py does this automatically) to obtain that cookie.

Endpoints:
  GET /systems/{id}/energy_lifetime     -> production[] (daily Wh)
  GET /systems/{id}/consumption_lifetime -> consumption[] (daily Wh)

Only stores complete past days (up to and including yesterday).
Today's incomplete data is never written.

Output: data/enphase_daily.csv  columns: date, production_kwh, consumption_kwh
"""

from __future__ import annotations

import json
import sys
import urllib.request
from datetime import datetime, timedelta
from pathlib import Path

import pandas as pd

PROJECT_DIR = Path(__file__).parent          # py_programs/
ROOT_DIR    = PROJECT_DIR.parent             # BatterySolarMonitorUpdate/
DATA_DIR    = ROOT_DIR / "data"
# Both one level above BatterySolarMonitorUpdate/ (i.e. ../ from ROOT_DIR)
# -- CONFIG_FILE is site_settings.csv, this whole pipeline's identity
# config (human-managed, required; see auth/enphase_token.py's own module
# docstring for why it's this file and not a separate one); SESSION_FILE
# is regenerated fresh by auth/enphase_token.py every run and deleted by
# quarterly_update.py once the whole run finishes.
CONFIG_FILE  = ROOT_DIR.parent / "site_settings.csv"
SESSION_FILE = ROOT_DIR.parent / "_session.csv"
OUT_PATH     = DATA_DIR / "enphase_daily.csv"


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

BASE_URL = "https://enlighten.enphaseenergy.com"


def enlighten_headers(session: str) -> dict:
    """Return authentication headers for the Enlighten web API."""
    return {
        "Cookie":           f"_enlighten_4_session={session}",
        "e-auth-token":     session,
        "Accept":           "application/json",
        "User-Agent":       "Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
        "X-Requested-With": "XMLHttpRequest",
    }


def _fetch_lifetime(endpoint: str, key: str, session: str,
                    system_id: str) -> dict[str, float] | None:
    """
    Fetch a lifetime daily series from Enlighten.

    Parameters
    ----------
    endpoint : str
        API path suffix, e.g. 'energy_lifetime' or 'consumption_lifetime'.
    key : str
        JSON array key in the response, e.g. 'production' or 'consumption'.
    session : str
        Enlighten session cookie value.
    system_id : str
        Enlighten system ID.

    Returns
    -------
    dict mapping YYYY-MM-DD -> kWh for all complete days up to yesterday,
    or None if the request failed or the session has expired.
    """
    url = f"{BASE_URL}/systems/{system_id}/{endpoint}"
    req = urllib.request.Request(url, headers=enlighten_headers(session))
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            data = json.loads(r.read())
    except Exception as e:
        print(f"  {endpoint} fetch failed: {e}")
        return None

    if not isinstance(data, dict) or key not in data:
        print(f"  {endpoint}: unexpected response — session may have expired.")
        return None

    start = datetime.fromisoformat(
        data["start_date"].replace(".000", "")
    ).date()
    values = data[key]
    yesterday = datetime.now().date() - timedelta(days=1)
    print(f"  {endpoint}: {len(values)} days from {start}")

    result: dict[str, float] = {}
    for i, wh in enumerate(values):
        day = start + timedelta(days=i)
        if day > yesterday:
            break   # do not store today's incomplete data
        if wh and wh > 0:
            result[str(day)] = round(wh / 1000, 3)
    return result


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
        print("No enlighten_session in session file — skipping.")
        return

    # ── Load existing data ───────────────────────────────────────────────────
    existing: pd.DataFrame | None = None
    stored_dates: set[str] = set()
    if OUT_PATH.exists():
        existing = pd.read_csv(OUT_PATH, dtype={"date": str})
        stored_dates = set(existing["date"].unique())
        print(f"Existing enphase_daily.csv: {len(stored_dates)} days "
              f"({min(stored_dates)} to {max(stored_dates)})")

    # ── Fetch both series from Enlighten ─────────────────────────────────────
    print("Fetching production and consumption from Enlighten ...")
    production  = _fetch_lifetime("energy_lifetime",     "production",  session, system_id)
    consumption = _fetch_lifetime("consumption_lifetime", "consumption", session, system_id)

    if production is None and consumption is None:
        print("Both fetches failed — skipping update.")
        return

    # ── Build new rows for dates not already stored ──────────────────────────
    all_dates = set(production or {}) | set(consumption or {})
    new_dates  = sorted(all_dates - stored_dates)

    prod_new = len([d for d in new_dates if (production  or {}).get(d) is not None])
    cons_new = len([d for d in new_dates if (consumption or {}).get(d) is not None])
    print(f"  production:  {'skipping - already up to date' if prod_new == 0 else f'{prod_new} new day(s)'}")
    print(f"  consumption: {'skipping - already up to date' if cons_new == 0 else f'{cons_new} new day(s)'}")

    if not new_dates:
        return

    new_rows = []
    for d in new_dates:
        new_rows.append({
            "date":            d,
            "production_kwh":  (production  or {}).get(d, None),
            "consumption_kwh": (consumption or {}).get(d, None),
        })

    new_df = pd.DataFrame(new_rows)

    if existing is not None:
        combined = pd.concat([existing, new_df], ignore_index=True)
    else:
        combined = new_df

    combined = (combined
                .drop_duplicates(subset="date")
                .sort_values("date")
                .reset_index(drop=True))

    DATA_DIR.mkdir(exist_ok=True)
    combined.to_csv(OUT_PATH, index=False)
    print(f"Saved {len(combined)} days to {OUT_PATH}  "
          f"(+{len(new_dates)} new)")


if __name__ == "__main__":
    import traceback as _tb
    try:
        main()
    except Exception:
        print(f"UNHANDLED ERROR in {Path(__file__).name}:")
        print(_tb.format_exc())
        raise SystemExit(1)
