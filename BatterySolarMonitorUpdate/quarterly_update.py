#!/usr/bin/env python3
"""
Single entry point for the ~quarterly shading-model refresh cycle. See
docs/quarterly_update_process.md for the full runbook (when to run this,
how to read its output, what to check before flashing the ESP32).

Before any step runs, _check_config_csv() verifies ../site_settings.csv
exists and has every REQUIRED_CONFIG_KEYS value filled in -- all 13
fields, both the 5 Enphase IDENTITY ones (gateway_ip, enlighten_system_id,
enlighten_email, enlighten_password, gateway_serial) and the 8 SITE ones
(latitude, longitude, panel_tilt, panel_azimuth, system_kw,
posix_timezone, car_charger_ip, hot_water_ip -- the last two are
ESP32-only fields, unused by this pipeline, required here only so this
file's one shared schema doesn't trip the extra-key check below). Added
2026-09-27 so a missing/incomplete file stops the WHOLE run immediately
with one clear message, instead of surfacing piecemeal as a confusing mix
of [OK]/[FAILED] steps.

open_meteo_timezone (a 7th SITE field, an IANA name) was REMOVED
2026-09-28: Open-Meteo's archive API was found to apply a single fixed,
non-DST-aware UTC offset for an entire IANA-timezone request, so
fetch_solar_history.py now requests UTC and converts to local using
posix_timezone (dateutil.tz.tzstr()) instead -- see that script's own
module docstring for the full story. The ESP32's own SiteSettings.h/.cpp/
SolarForecastFetch.cpp were updated the same day to match (same fix,
timezone=UTC + localtime_r() using sitePosixTimezone() -- see
esp32_control_structure.md), so ../site_settings.csv and
BatterySolarMonitor/data/site_settings.csv are both 13 fields (5 identity
+ 8 site, including car_charger_ip/hot_water_ip) and stay in sync.

../site_settings.csv is a genuine, entirely human-maintained CONFIG file
-- nothing in this pipeline ever writes to it any more (changed
2026-09-27: an earlier version had a step here that regenerated the 7
SITE fields from fetch_solar_history.py's/solar_shading_model.py's own
LAT/LON/PANEL_TILT/PANEL_AZIMUTH/SYSTEM_KW constants every run, but those
are just as fixed/never-actually-changing as the 5 identity fields, so
there was nothing genuinely "regeneratable" about them either -- treating
them that way just meant two independently-editable sources of the same
physical facts that could silently drift out of sync). The direction is
now reversed: fetch_solar_history.py itself reads LAT/LON/PANEL_TILT/
PANEL_AZIMUTH straight from this file (and is THE Python-side source for
them -- every other script that needs them imports from there, not from
its own separate copy), and solar_shading_model.py reads SYSTEM_KW from
it directly. See
auth/site_settings.csv.sample for a fresh checkout: fill in all 13 rows
by hand; nothing derives any of them for you. `EnphaseAuth.h`'s/
`SiteSettings.h`'s own `loadEnphaseAuth()`/`loadSiteConfig()` on the ESP32
side load the exact same 13-field file at boot (copied into
BatterySolarMonitor/data/site_settings.csv by hand when it changes --
this pipeline never does that copy either now).
Deliberately excludes gateway_jwt/enlighten_session -- those are LIVE
TOKENS the device refreshes and persists itself once a day
(EnlightenLoginFetch.h) into its OWN separate data/auth_token.csv on
LittleFS, so a PC-side copy would just be instantly-stale busywork; see
esp32_control_structure.md §1/§1a for the full identity-vs-live-token
split.

Steps, in order:
  1. auth/enphase_token.py's get_session() -- log into Enlighten and get a
     fresh session cookie this PC-side pipeline needs for steps 3/4's own
     Enlighten Cloud API calls, saved to ../_session.csv (a separate,
     disposable file -- NOT ../site_settings.csv -- see that script's own
     module docstring). Always does a real login, every run, with no
     caching: this pipeline only runs about once every 3 months, far
     longer than the cookie's own ~14-day lifetime, so a cached value would
     essentially never still be valid anyway. (Until 2026-09-27 this step
     also fetched a JWT for the local gateway -- that was PC-internal only
     since 2026-09-10 and dropped entirely once it turned out nothing in
     this pipeline ever read it back.) ../_session.csv is deleted at the
     end of this run -- see main()'s own final step.
  2. fetch_solar_history.py       -- Open-Meteo hourly GTI archive
  3. fetch_enlighten_daily.py     -- Enphase daily production/consumption
  4. fetch_consumption_15min.py   -- Enlighten 15-min solar/consumption/
                                      battery/grid history
  5. solar_shading_model.py       -- refit shade factors, regenerate
                                      data/shade_grid.csv and the other
                                      calibration outputs
  6. Copy the new data/shade_grid.csv AND data/morning_floor.csv to
     ../shade_grid.csv / ../morning_floor.csv (one level above
     BatterySolarMonitorUpdate/), alongside ../site_settings.csv -- so all
     three files the ESP32 needs sit together in one folder for a single
     manual copy. Neither file is copied into
     BatterySolarMonitor/data/ automatically (changed back 2026-09-28, per
     explicit instruction -- a same-day earlier version of this step DID
     copy both there directly; reverted so the ESP32 sketch's own data
     folder is updated by a deliberate, explicit manual step every time,
     same convention site_settings.csv has always used, rather than a
     background script silently overwriting it on every run).
  7. plot_gti_vs_model.py --days 7 -- regenerates
     docs/charts/gti_vs_model.png (raw Open-Meteo GTI vs shading-corrected
     model vs actual production) for the most recent 7 days actually
     present in data/solar_15min_history.csv. Added 2026-09-28 -- this
     chart used to only update when someone remembered to run the script
     by hand, so it could silently go stale for weeks.
  8. analyse_tree_shadow.py       -- compares accumulated WNW-zone hourly
                                      evidence (data/tree_shadow_calibration.csv)
                                      against the current _TREE_SF_PROFILE and
                                      prints suggested updates, if any. Never
                                      edits solar_shading_model.py itself --
                                      review the output and apply by hand.

Each step's stdout/stderr is shown live and also appended to
docs/charts/quarterly_update.log. A step failing does not stop the ones
after it (matching update_daily.bat's own "continue with existing data"
behaviour for fetch failures) -- except that if solar_shading_model.py
itself fails, its output isn't there to copy or analyse, so step 6's copy
is skipped and step 7 will likely fail too (logged, not fatal).

Usage (run from BatterySolarMonitorUpdate/, not py_programs/ -- moved here
2026-09-28 for ease of identification as the pipeline's single entry point):
    python3 quarterly_update.py
"""

from __future__ import annotations

import subprocess
import sys
import shutil
from datetime import datetime
from pathlib import Path

BASE_DIR    = Path(__file__).parent           # BatterySolarMonitorUpdate/
# Moved here from py_programs/ 2026-09-28, for ease of identification as
# the pipeline's single entry point -- the actual fetch/refit scripts this
# file runs (via _run_script()) still live in py_programs/, so ROOT_DIR is
# now derived rather than being __file__'s own directory.
ROOT_DIR    = BASE_DIR / "py_programs"        # py_programs/
# One level above BatterySolarMonitorUpdate/ (i.e. ../ from BASE_DIR) --
# private credentials/generated-settings files live here (moved
# 2026-09-27), never inside a tracked/shareable project source tree.
PRIVATE_DIR = BASE_DIR.parent
LOG_PATH    = BASE_DIR / "docs" / "charts" / "quarterly_update.log"

# shade_grid.csv/morning_floor.csv -- both regenerated by
# solar_shading_model.py's refit (step 5) every run, both ESP32-consumed
# (ShadeFactorTable.cpp/MorningFloorTable.cpp), NEITHER copied into
# BatterySolarMonitor/data/ by this script (changed 2026-09-28, per
# explicit instruction -- see the module docstring's own step 6
# explanation). Both land one level above BatterySolarMonitorUpdate/
# instead, alongside site_settings.csv (SITE_SETTINGS_DST, below), so all
# three files the ESP32 needs for its next sync+flash sit together in one
# folder for a single manual copy -- same by-hand convention
# site_settings.csv has always used, now applied consistently to all
# three rather than mixing "some auto-copied, some manual".
SHADE_SRC   = BASE_DIR / "data" / "shade_grid.csv"
SHADE_PRIVATE_DST = PRIVATE_DIR / "shade_grid.csv"
MORNING_FLOOR_SRC = BASE_DIR / "data" / "morning_floor.csv"
MORNING_FLOOR_PRIVATE_DST = PRIVATE_DIR / "morning_floor.csv"

# site_settings.csv moved 2026-09-27, out of BatterySolarMonitor/data/ --
# it contains the plaintext Enlighten account password among other
# identity fields, so it must not sit inside the ESP32 sketch's own
# tracked/shareable data folder. Copy PRIVATE_DIR/site_settings.csv (and,
# since 2026-09-28, PRIVATE_DIR/shade_grid.csv and
# PRIVATE_DIR/morning_floor.csv, sitting right next to it) into
# BatterySolarMonitor/data/ by hand, right before an actual firmware+data
# upload.
#
# SITE_SETTINGS_DST is a genuine, entirely human-maintained config file --
# nothing in this pipeline writes to it any more (changed 2026-09-27; see
# the module docstring's own explanation of why even the 7 SITE fields
# turned out not to be worth "regenerating" every run). SESSION_CSV is the
# opposite: pure disposable run state, written fresh by
# auth/enphase_token.py every run and deleted by this script's own main()
# once the whole run finishes -- see that script's own module docstring
# for the full reasoning.
SESSION_CSV = PRIVATE_DIR / "_session.csv"

# All 13 fields -- nothing in this pipeline derives or fetches any of
# them; every one must be seeded by hand (see auth/site_settings.csv.sample)
# before ANY step can do anything useful. open_meteo_timezone removed
# 2026-09-28 (see module docstring) -- do not re-add without also fixing
# fetch_solar_history.py's fetch_chunk_utc() back to an IANA name, which
# would reintroduce the DST bug that removal fixed.
REQUIRED_CONFIG_KEYS = [
    "gateway_ip", "enlighten_system_id", "enlighten_email",
    "enlighten_password", "gateway_serial",
    "latitude", "longitude", "panel_tilt", "panel_azimuth",
    "system_kw", "posix_timezone",
    # car_charger_ip/hot_water_ip: ESP32-only fields (CarChargerFetch.cpp/
    # HotWaterFetch.cpp) -- nothing in this Python pipeline reads them, but
    # they're part of this file's one shared schema (see
    # esp32_control_structure.md section 1a), so they're required here
    # too, purely so this strict key-set check doesn't misflag them as an
    # unexpected extra key.
    "car_charger_ip", "hot_water_ip",
]

SITE_SETTINGS_DST = PRIVATE_DIR / "site_settings.csv"

PYTHON = sys.executable


def _log(line: str) -> None:
    print(line)
    LOG_PATH.parent.mkdir(parents=True, exist_ok=True)
    with open(LOG_PATH, "a", encoding="utf-8") as f:
        f.write(line + "\n")


def _check_config_csv() -> bool:
    """
    Verify SITE_SETTINGS_DST exists, has every REQUIRED_CONFIG_KEYS value
    non-empty, and has NO other keys -- BEFORE running any step. Added
    2026-09-27, after a fresh checkout with a missing/incomplete config
    file produced a confusing mix of [OK]/[FAILED] steps (some steps need
    the file, some don't, so a missing file used to only surface piecemeal
    as each affected step ran). One clear message, up front, stops the
    whole run immediately instead.

    If the file is missing entirely, generates a blank template there
    (every REQUIRED_CONFIG_KEYS row present, value empty) rather than just
    pointing at auth/site_settings.csv.sample for the user to copy by
    hand -- one less manual step for a fresh checkout. This is the ONE
    exception to "nothing in this pipeline writes to SITE_SETTINGS_DST"
    (see the module docstring): there is no existing human-entered data to
    clobber when the file doesn't exist at all, so writing an empty
    skeleton doesn't violate that rule. Still fails the check immediately
    afterward, since the freshly-written values are all blank -- the user
    must fill them in and re-run.

    The extra-keys check (2026-09-27) catches typos/renamed keys that
    would otherwise silently read back as empty via .get() and produce a
    confusing "missing" error that doesn't match what's visibly in the
    file -- e.g. a leftover 'system_id' row (the old _auth_token.csv name)
    sitting alongside a correctly-named but now-unfilled
    'enlighten_system_id' row.
    """
    if not SITE_SETTINGS_DST.exists():
        SITE_SETTINGS_DST.parent.mkdir(parents=True, exist_ok=True)
        # Plain string writing, NOT csv.writer() -- see
        # auth/enphase_token.py's own _save_kv_csv() for why (this project's
        # simple first-comma-split convention, not proper quoted CSV).
        with open(SITE_SETTINGS_DST, "w", newline="") as f:
            f.write("key,value\n")
            for k in REQUIRED_CONFIG_KEYS:
                f.write(f"{k},\n")
        _log(f"ERROR: {SITE_SETTINGS_DST} not found -- generated a blank "
             f"template there with all {len(REQUIRED_CONFIG_KEYS)} required "
             f"keys (see auth/site_settings.csv.sample for field meanings). "
             f"Fill in the values and re-run. Stopping -- no steps run.")
        return False

    sys.path.insert(0, str(ROOT_DIR / "auth"))
    from enphase_token import load_config  # noqa: E402  -- reads SITE_SETTINGS_DST

    config = load_config()
    required = set(REQUIRED_CONFIG_KEYS)
    present  = set(config.keys())
    missing_keys = sorted(required - present)
    extra_keys   = sorted(present - required)
    empty_values = sorted(k for k in REQUIRED_CONFIG_KEYS if k in config and not config[k])

    if missing_keys or extra_keys or empty_values:
        problems = []
        if missing_keys:
            problems.append(f"missing key(s) {missing_keys}")
        if extra_keys:
            problems.append(f"unexpected extra key(s) {extra_keys}")
        if empty_values:
            problems.append(f"empty value(s) for {empty_values}")
        _log(f"ERROR: {SITE_SETTINGS_DST}: " + "; ".join(problems) +
             " -- see auth/site_settings.csv.sample. Stopping -- no steps run.")
        return False
    return True


def _run_script(rel_path: str, *args: str) -> bool:
    """Run one script with the current interpreter; stream output; return success."""
    script_path = ROOT_DIR / rel_path
    _log(f"=== {rel_path} {' '.join(args)} {datetime.now().isoformat(timespec='seconds')} ===")
    result = subprocess.run([PYTHON, str(script_path), *args],
                             cwd=str(script_path.parent),
                             capture_output=True, text=True)
    if result.stdout:
        _log(result.stdout.rstrip("\n"))
    if result.stderr:
        _log(result.stderr.rstrip("\n"))
    ok = result.returncode == 0
    _log(f"--- {rel_path}: {'OK' if ok else f'FAILED (exit {result.returncode})'} ---\n")
    return ok


def main() -> None:
    if not _check_config_csv():
        raise SystemExit(1)

    summary: list[tuple[str, bool]] = []

    ok = _run_script("auth/enphase_token.py")
    summary.append(("log into Enlighten, save session", ok))
    if not ok:
        _log("WARNING: login failed -- fetch steps below will likely fail too, "
             "since there's no cached session to fall back on any more.")

    ok = _run_script("fetch_solar_history.py")
    summary.append(("fetch_solar_history.py", ok))

    ok = _run_script("fetch_enlighten_daily.py")
    summary.append(("fetch_enlighten_daily.py", ok))

    ok = _run_script("fetch_consumption_15min.py")
    summary.append(("fetch_consumption_15min.py", ok))

    ok = _run_script("solar_shading_model.py")
    summary.append(("solar_shading_model.py (refit)", ok))

    if ok:
        if SHADE_SRC.exists():
            shutil.copyfile(SHADE_SRC, SHADE_PRIVATE_DST)
            _log(f"Copied {SHADE_SRC} -> {SHADE_PRIVATE_DST}")
            summary.append(("copy shade_grid.csv to PRIVATE_DIR", True))
        else:
            _log(f"ERROR: {SHADE_SRC} not found after refit -- nothing copied.")
            summary.append(("copy shade_grid.csv to PRIVATE_DIR", False))

        if MORNING_FLOOR_SRC.exists():
            shutil.copyfile(MORNING_FLOOR_SRC, MORNING_FLOOR_PRIVATE_DST)
            _log(f"Copied {MORNING_FLOOR_SRC} -> {MORNING_FLOOR_PRIVATE_DST}")
            summary.append(("copy morning_floor.csv to PRIVATE_DIR", True))
        else:
            _log(f"ERROR: {MORNING_FLOOR_SRC} not found after refit -- nothing copied.")
            summary.append(("copy morning_floor.csv to PRIVATE_DIR", False))
    else:
        _log(f"Refit failed -- skipping shade_grid.csv/morning_floor.csv copies. "
             f"{SHADE_PRIVATE_DST} and {MORNING_FLOOR_PRIVATE_DST} are UNCHANGED.")
        summary.append(("copy shade_grid.csv to PRIVATE_DIR", False))
        summary.append(("copy morning_floor.csv to PRIVATE_DIR", False))

    if ok:
        chart_ok = _run_script("plot_gti_vs_model.py", "--days", "7")
        summary.append(("plot_gti_vs_model.py (gti_vs_model.png, last 7 days)", chart_ok))
    else:
        _log("Refit failed -- skipping gti_vs_model.png "
             "(calibrated_solar_hourly_v2.csv wasn't regenerated).")
        summary.append(("plot_gti_vs_model.py (gti_vs_model.png, last 7 days)", False))

    ok = _run_script("analyse_tree_shadow.py")
    summary.append(("analyse_tree_shadow.py (WNW profile check)", ok))
    if ok:
        _log("Review the printed gap table above -- if it suggests changes, they are "
             "NOT applied automatically (same as the fetch/refit steps do apply "
             "themselves): edit _TREE_SF_PROFILE/_TREE_EL_ONSET in "
             "solar_shading_model.py by hand and re-run.")

    _log("\n=== Summary ===")
    all_ok = True
    for name, step_ok in summary:
        _log(f"  [{'OK' if step_ok else 'FAILED'}] {name}")
        all_ok = all_ok and step_ok

    if all_ok:
        _log("\nAll steps succeeded. Review docs/solar_shading_stats.md and the "
             "override-table diff lines above before syncing + flashing the ESP32 "
             "-- see docs/quarterly_update_process.md.")
    else:
        _log("\nOne or more steps failed -- see above. If solar_shading_model.py "
             "itself succeeded despite an earlier fetch failure, shade_grid.csv "
             "was still updated using whatever data was already present.")

    # SESSION_CSV is pure disposable run state (see its own comment above) --
    # delete it now that the whole run has finished, regardless of outcome,
    # rather than leaving a stale session sitting on disk until next month.
    if SESSION_CSV.exists():
        SESSION_CSV.unlink()
        _log(f"Deleted {SESSION_CSV} (disposable -- regenerated fresh next run).")

    # Explicit, deliberately separate closing reminder -- per explicit
    # instruction (2026-09-28) -- that syncing the ESP32's own data folder
    # is ENTIRELY a manual step now: this pipeline writes site_settings.csv,
    # shade_grid.csv and morning_floor.csv only to PRIVATE_DIR (one level
    # above BatterySolarMonitorUpdate/), never into
    # BatterySolarMonitor/data/ itself, regardless of outcome above.
    _log("\n\nMANUAL STEP REQUIRED: copy "
         f"{SITE_SETTINGS_DST}, {SHADE_PRIVATE_DST} and {MORNING_FLOOR_PRIVATE_DST} "
         "into BatterySolarMonitor/data/ by hand before the next firmware+data "
         "upload -- this pipeline never writes to BatterySolarMonitor/data/ "
         "itself.")

    raise SystemExit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
