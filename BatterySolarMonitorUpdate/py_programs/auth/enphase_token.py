#!/usr/bin/env python3
"""
Log into Enlighten and get a fresh session cookie for this pipeline's own
Cloud API calls (fetch_enlighten_daily.py, fetch_consumption_15min.py).

Identity config (enlighten_email, enlighten_password, gateway_ip,
enlighten_system_id, gateway_serial -- 5 of its 13 fields; the other 8 are
site/location/network config this script doesn't need) lives in
../site_settings.csv (one level above BatterySolarMonitorUpdate/, so
these credentials are never inside a tracked/shareable project source
tree) -- the SAME file the ESP32's own EnphaseAuth.h/SiteSettings.h load
at boot. Entirely human-maintained: NOTHING in this pipeline writes to
it any more (changed 2026-09-27 -- see quarterly_update.py's own module
docstring for why even its 7 site/location fields turned out not to be
worth "regenerating" every run). Merged 2026-09-27 from a separate
_auth_token.csv that had drifted into duplicating most of the same
identity data under different key names. This script only ever READS the
file. See site_settings.csv.sample (this same directory) for the format;
copy it to ../site_settings.csv and fill in all 13 rows by hand for
first-time setup.

Always does a real login and writes the resulting _enlighten_4_session
cookie to a SEPARATE file, ../_session.csv -- deliberately not cached or
reused across runs: this pipeline only runs about once every 3 months,
far longer than the cookie's own ~14-day lifetime, so a cached value
would essentially never still be valid anyway, and mixing
regenerate-every-run state into the config file made it unclear which
fields were real (human-managed) config and which were leftover run
state. ../_session.csv exists only so fetch_enlighten_daily.py/
fetch_consumption_15min.py -- separate subprocesses started later in the
same quarterly_update.py run -- can read the session this step just
obtained; quarterly_update.py deletes it once the whole run finishes.
Treat it as disposable: safe to delete any time, and always overwritten
fresh at the start of every run.

(Until 2026-09-27 this script also fetched a JWT for the LOCAL gateway,
cached as "jwt_token" -- removed because nothing in this pipeline ever
read it back; only enlighten_session is actually used. The ESP32 firmware
performs its own, entirely separate Enlighten login for its own JWT
needs -- see esp32_control_structure.md -- and never reads anything this
script writes.)

Usage:
    python3 auth/enphase_token.py

Works on macOS, Linux, and Windows. No pip packages required.
"""

from __future__ import annotations

import http.cookiejar
import json
import sys
import urllib.parse
import urllib.request
from pathlib import Path

# auth/ is two levels below BatterySolarMonitorUpdate/ (BatterySolarMonitorUpdate/py_programs/auth/)
PROJECT_DIR = Path(__file__).parent          # py_programs/auth/
ROOT_DIR    = PROJECT_DIR.parent.parent      # BatterySolarMonitorUpdate/
# One level above BatterySolarMonitorUpdate/ (i.e. ../ from ROOT_DIR).
PRIVATE_DIR = ROOT_DIR.parent
# The whole pipeline's identity/site config -- entirely human-managed,
# never written by any script. See the module docstring and
# site_settings.csv.sample for the required fields.
CONFIG_CSV  = PRIVATE_DIR / "site_settings.csv"
# Regenerated every run -- disposable, deleted by quarterly_update.py once
# the whole run finishes. See module docstring.
SESSION_CSV = PRIVATE_DIR / "_session.csv"


def _login(email: str, password: str) -> str:
    """
    POST credentials to Enlighten and return the _enlighten_4_session cookie.

    Uses an http.cookiejar.CookieJar so the cookie set by the login
    response is captured automatically -- no manual header parsing needed.

    Parameters
    ----------
    email : str
        Enlighten account email address.
    password : str
        Enlighten account password.

    Returns
    -------
    str
        Value of the _enlighten_4_session web cookie, or "" if the cookie
        was not found in the response.
    """
    cookie_jar = http.cookiejar.CookieJar()
    opener = urllib.request.build_opener(
        urllib.request.HTTPCookieProcessor(cookie_jar)
    )
    login_data = urllib.parse.urlencode({
        "user[email]": email,
        "user[password]": password,
    }).encode()
    req = urllib.request.Request(
        "https://enlighten.enphaseenergy.com/login/login.json",
        data=login_data,
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )
    with opener.open(req, timeout=15) as resp:
        json.loads(resp.read().decode())  # raises if login itself failed
    enlighten_session = next(
        (c.value for c in cookie_jar if c.name == "_enlighten_4_session"), ""
    )
    if enlighten_session:
        print("Captured _enlighten_4_session cookie.")
    else:
        print("Warning: _enlighten_4_session cookie not found in login response.")
    return enlighten_session


def _load_kv_csv(path: Path) -> dict[str, str]:
    """
    Load a key,value CSV (header first) into a dict. Empty dict if missing.

    Splits each line on its FIRST comma only -- NOT a proper quoted-CSV
    parse (csv.reader() splits on every comma, which is wrong here: fixed
    2026-09-27 after it silently truncated posix_timezone's own value, a
    POSIX TZ string that itself contains commas, e.g.
    "AEST-10AEDT,M10.1.0,M4.1.0/3", down to just "AEST-10AEDT"). Matches
    BatterySolarMonitor/SiteSettings.cpp's own loadSiteConfig() exactly,
    including its whole-line strip() and its separate post-split strip()
    of the value (added 2026-09-27, after a hand-edited-style
    "key, value" row -- a space after the comma, an entirely natural thing
    for a person editing this file by hand to type -- was confirmed to
    silently corrupt the value with a leading space, e.g. enlighten_email
    becoming " someone@example.com"): this whole project's established
    "simple CSV, not JSON, no quoting" convention.
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


def _save_kv_csv(path: Path, data: dict[str, str]) -> None:
    """
    Write `data` to `path` as key,value rows, creating PRIVATE_DIR if
    needed. Plain string writing, NOT csv.writer() -- csv.writer() would
    quote any value containing a comma (e.g. posix_timezone's own POSIX TZ
    string), which the simple first-comma-split parsers this project uses
    (see _load_kv_csv()'s own comment) do not understand. No value in this
    project's own data is ever expected to contain a bare newline, which
    is the only thing this simpler format genuinely can't represent.
    """
    PRIVATE_DIR.mkdir(parents=True, exist_ok=True)
    with open(path, "w", newline="") as f:
        f.write("key,value\n")
        for k, v in data.items():
            f.write(f"{k},{v}\n")


def load_config() -> dict[str, str]:
    """Load CONFIG_CSV -- all 13 site_settings.csv fields, though this script only uses the 5 identity ones (see module docstring)."""
    return _load_kv_csv(CONFIG_CSV)


def save_session(session: dict[str, str]) -> None:
    """Write `session` to SESSION_CSV, overwriting whatever was there before."""
    _save_kv_csv(SESSION_CSV, session)


def get_session() -> str:
    """
    Log into Enlighten and return a fresh _enlighten_4_session cookie,
    also saving it to SESSION_CSV for the later fetch steps to read.
    Always does a real login -- see module docstring for why this is no
    longer cached/reused across runs.

    Returns
    -------
    str
        The _enlighten_4_session cookie value.
    """
    config = load_config()
    email    = config.get("enlighten_email", "")
    password = config.get("enlighten_password", "")

    if not email or not password:
        print("ERROR: config CSV must contain 'enlighten_email' and 'enlighten_password' rows.")
        print(f"  Looking at: {CONFIG_CSV} (see site_settings.csv.sample in this directory)")
        sys.exit(1)

    print("Logging into Enlighten...")
    enlighten_session = _login(email, password)

    if not enlighten_session:
        print("ERROR: login succeeded but no _enlighten_4_session cookie was returned.")
        sys.exit(1)

    save_session({"enlighten_session": enlighten_session})
    print(f"Session saved to {SESSION_CSV}")
    return enlighten_session


def main() -> None:
    session = get_session()
    # Show first/last few chars so user can verify it's real
    print(f"Session: {session[:8]}...{session[-8:]}")
    print(f"Length: {len(session)} chars")


if __name__ == "__main__":
    main()
