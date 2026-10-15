# Quarterly Shading-Model Update Process

## Contents

1. Purpose  
2. When to run it  
3. What to run  
4. How to read the output  
5. Manual steps — not automated  
6. Last step — sync and flash

---

## Purpose

The ESP32 (`BatterySolarMonitor`) does its own live solar forecasting
(`SolarForecastFetch.cpp`) and its own car/hot-water load control
(`LoadControlTask.cpp`) — see [`esp32_control_structure.md`](esp32_control_structure.md).
The one thing it can't do itself is figure out how much the panels are
actually shaded at each sun position: that requires a regression against
months/years of real production history, which only makes sense to run on
a PC, not the microcontroller.

This directory (`BatterySolarMonitorUpdate/`) holds that regression
pipeline's entry point (`quarterly_update.py`), its data (`data/`), and
these docs; the individual fetch/refit scripts it runs
(`fetch_solar_history.py`, `solar_shading_model.py`, and the rest) live
in its own `py_programs/` subdirectory.

**Output:** `data/shade_grid.csv` — a lookup table of `(az_bin, el_bin) ->
shade_factor`, loaded at boot by `ShadeFactorTable.cpp` — and
`data/morning_floor.csv` — a `(month, hour) -> clear-sky floor kW` lookup,
loaded by `MorningFloorTable.cpp` the same way. Both are copied to
`../shade_grid.csv`/`../morning_floor.csv` (one level above
`BatterySolarMonitorUpdate/`), never straight into
`BatterySolarMonitor/data/` — see "Last step — sync and flash" below for
why and how they get there.

## When to run it

Every **~3 months**, or sooner if:
- A season change is coming up that a known-thin part of the model hasn't
  seen real data for yet (e.g. summer mornings — see the Easterly Shading
  section in [`solar_shading_model.md`](solar_shading_model.md)).
- You've noticed the Status screen's shading-adjusted forecast looking
  systematically off for a stretch of clear days.

There's no harm in running it more often — the fetch scripts are all
incremental (only pull days not already stored) and cheap.

## What to run

From `BatterySolarMonitorUpdate/` itself, the pipeline's single entry
point, for ease of identification (the individual fetch/refit scripts it
calls live in `py_programs/`):

```
cd BatterySolarMonitorUpdate
python quarterly_update.py
```

This single script does everything below, printing each step's result and
appending to `docs/charts/quarterly_update.log`:

**Before any step runs**: checks that `../site_settings.csv`
— a genuine, entirely human-maintained CONFIG file; nothing in this
pipeline ever writes to it — exists and has all 13 fields filled in:
   - 5 IDENTITY fields (`gateway_ip`, `enlighten_system_id`,
     `enlighten_email`, `enlighten_password`, `gateway_serial` — fixed
     account/hardware config the device can't derive itself). The ESP32's
     own `EnphaseAuth.h` loads these at boot.
   - 8 SITE CONFIG fields (`latitude`, `longitude`, `panel_tilt`,
     `panel_azimuth`, `system_kw`, `posix_timezone`, `car_charger_ip`,
     `hot_water_ip` — fixed physical/location/network config). The ESP32's
     own `SiteSettings.h` loads these at boot; `car_charger_ip`/
     `hot_water_ip` are the local LAN IPs `CarChargerFetch.cpp`/
     `HotWaterFetch.cpp` connect to directly — required here (this Python
     pipeline never reads them itself) purely so this file's one shared
     schema doesn't trip the extra-key check below.

   `open_meteo_timezone` (a would-be 7th SITE field, an IANA name) is not
   part of this file: Open-Meteo's archive API applies a single fixed,
   non-DST-aware UTC offset for an entire IANA-timezone request, so
   `fetch_solar_history.py` instead requests UTC and converts to local
   using `posix_timezone` (see [`data_sources.md`](data_sources.md)'s
   "Timezone handling" section for the full story). The ESP32's own
   `SiteSettings.h`/`SolarForecastFetch.cpp` match (see
   [`esp32_control_structure.md`](esp32_control_structure.md)), so both
   copies of `site_settings.csv` are 13 fields and stay in sync.

  (The physical/location SITE fields are not regenerated each run from
  `fetch_solar_history.py`'s/`solar_shading_model.py`'s own
  `LAT`/`LON`/`PANEL_TILT`/`PANEL_AZIMUTH`/`SYSTEM_KW` constants: those
  values are just as fixed/never-actually-changing as the identity
  fields, so "regenerating" them each run would just mean two
  independently-editable sources of the same physical facts that could
  silently drift apart. Deliberately excludes `gateway_jwt`/
  `enlighten_session` — those are live tokens the device refreshes and
  persists itself once a day into its own separate `auth_token.csv` on
  LittleFS, see [`esp32_control_structure.md`](esp32_control_structure.md)
  §1/§1a.)

  If the file is missing entirely, a blank template (all 13 keys present,
  values empty) is generated at `../site_settings.csv` automatically — one
  less manual step than copying `py_programs/auth/site_settings.csv.sample`
  by hand — but the run still stops immediately, since the freshly-written
  values are all blank. If the file exists but is incomplete, checks the
  key SET exactly: reports any missing key, any unexpected EXTRA key (e.g.
  a leftover `system_id` row, or `open_meteo_timezone`), and any
  present-but-empty value, all together in one message.
  Either way, no steps run at all — instead of surfacing piecemeal as a
  confusing mix of `[OK]`/`[FAILED]` steps. **First-time setup / fresh
  checkout**: after the template is generated (or from
  `py_programs/auth/site_settings.csv.sample` directly), fill in all 13
  rows by hand (`gateway_ip`/`enlighten_system_id`/`gateway_serial` via the
  Enlighten dashboard / your gateway's own local IP / its serial number
  sticker; `car_charger_ip`/`hot_water_ip` from your own LAN — check your
  router's client list or each device's own display; the other 6 SITE
  fields are fixed physical facts about your installation — nothing
  fetches or derives any of them). **Remember to
  also copy it into `BatterySolarMonitor/data/site_settings.csv` by hand**
  whenever you change it, before the next firmware+data upload — this
  pipeline never does that copy either.

1. **Log into Enlighten** (`auth/enphase_token.py`). Always does a real
   login, never a cached one: this pipeline only runs about once every 3
   months, far longer than the session cookie's own ~14-day lifetime, so
   a cached value would essentially never still be valid anyway. Reads
   `enlighten_email`/`enlighten_password` from `../site_settings.csv` and
   saves the resulting session cookie to a SEPARATE file,
   `../_session.csv` — disposable, regenerated every run, deleted once
   the whole `quarterly_update.py` run finishes (see the very end of this
   list) so steps 3/4 (separate subprocesses started later in the same
   run) can read the session this step just obtained. If login fails,
   check `../site_settings.csv` and that the account itself is still
   valid (no 2FA change, etc.). (This step does not fetch a JWT — JSON
   Web Token, the local gateway's own access-token format — for the
   local gateway; nothing in this pipeline reads one back. The ESP32
   firmware performs its own, entirely separate login for its own JWT
   needs — see `esp32_control_structure.md` §1.)
2. **`fetch_solar_history.py`** — pulls new Open-Meteo hourly GTI/cloud
   history since the last stored date.
3. **`fetch_enlighten_daily.py`** — pulls new Enphase daily production/
   consumption totals.
4. **`fetch_consumption_15min.py`** — pulls new 15-min solar/consumption/
   battery/grid history from Enlighten. This is the one that actually lets
   the model resolve individual sun-position bins rather than just daily
   totals (see [`solar_shading_model.md`](solar_shading_model.md)'s
   "Shade Factor Regression" section). Handles the two Australian DST
   transition days automatically (92-slot October / 100-slot April) —
   see that script's own docstring for how.
5. **`solar_shading_model.py`** — re-solves the NNLS regression, applies
   the tree-shadow corrections and empirical overrides, regenerates
   `data/shade_grid.csv`, `data/calibrated_solar_hourly_v2.csv`,
   `data/morning_floor.csv`, `docs/solar_shading_stats.md`, and
   `docs/charts/solar_shading_model.png`.
6. **Copies `data/shade_grid.csv` and `data/morning_floor.csv`** to
   `../shade_grid.csv` / `../morning_floor.csv` (one level above
   `BatterySolarMonitorUpdate/`), alongside `../site_settings.csv` — never
   straight into `BatterySolarMonitor/data/` (see "Last step — sync and
   flash" below; that folder is only ever updated by a deliberate manual
   step, the same convention `site_settings.csv` uses, so a background
   script run never silently overwrites it).
7. **`plot_gti_vs_model.py --days 7`** — regenerates
   `docs/charts/gti_vs_model.png` for the most recent 7 days actually
   present in `data/solar_15min_history.csv` (runs automatically here
   rather than only when someone remembers to run the script by hand, so
   it can't silently go stale for weeks; see "How to read the output"
   below for what it shows).
8. **`analyse_tree_shadow.py`** — compares accumulated WNW-zone hourly
   evidence against the current `_TREE_SF_PROFILE` and prints suggested
   updates, if any (see "Manual steps" below — it never edits
   `solar_shading_model.py` itself).

**After every step finishes**, regardless of outcome: deletes
`../_session.csv` (step 1's disposable session file — see step 1's own
note) so a stale session never lingers on disk until next month; freshly
regenerated by step 1 on the very next run.

If a fetch step fails (network hiccup, expired session), the script
continues to the next step using whatever data is already stored. If
`solar_shading_model.py` itself fails, there's
nothing new to copy or analyse, so step 6 is skipped and
`../shade_grid.csv`/`../morning_floor.csv` are left untouched (step 7's
chart would just repeat the previous run's data, so it's skipped too;
step 8 will likely fail too, logged but not fatal).

## How to read the output

- **Fit quality**: `solar_shading_model.py` prints `RMSE`/`MAE`/`R2` for
  the NNLS fit against daily totals, and `docs/solar_shading_stats.md` has
  the full breakdown (seasonal RMSE/MAE/bias, shading hot-spots, clear-sky
  zones). Compare against the previous run's numbers (this doc's own
  history, or just re-read the file's git-free previous copy if you kept
  one) — a big regression in RMSE/R² after a run is worth investigating
  before flashing.
- **Override-table diff lines**: the "Applying empirical (az,el) shade
  overrides..." section prints `old -> new` for every bin in
  `_OBSERVED_SHADE_OVERRIDES`. A value that barely moved means that bin's
  calibration is stable; a big jump means recent data disagrees with the
  existing override — worth a manual look (see below) before trusting it
  blindly.
- **Sanity-check before flashing**: open `docs/charts/solar_shading_model.png`
  — the polar shading map (panel A) and azimuth/elevation profiles
  (panels C/D) should still look like a coherent shadow pattern (smooth-ish,
  physically plausible), not a scatter of isolated spikes. If something
  looks wrong, check `docs/charts/quarterly_update.log` for warnings first.
- **`docs/charts/gti_vs_model.png`** (step 7, generated automatically): a
  per-day chart (raw GTI vs shading-corrected model vs actual, one panel
  per day) for the most recent 7 available days — a closer look than the
  polar/grid summary at how well the model is tracking real production
  right now. Run `python plot_gti_vs_model.py --days N` (or `--start`/
  `--end`) by hand for a different window than the automatic last-7-days
  one.

## Manual steps — not automated

`quarterly_update.py` re-solves the model and re-applies whatever's
already in `_OBSERVED_SHADE_OVERRIDES`, but it does **not** decide whether
new bins should be added to that table, or existing ones adjusted beyond
what the NNLS solve + existing corrections already do. That's a judgment
call, done the same way the east-tree overrides were derived — see
[`solar_shading_model.md`](solar_shading_model.md)'s Easterly Shading
section for the full worked example:

1. Run `python diag_bin_dates.py` (or `diag_morning_overrides.py` /
   `diag_afternoon.py` / `diag_morning.py` for other angles on the same
   question) to see per-`(az_bin, el_bin)` implied shade factors, sample
   counts, and IQR.
2. If a zone looks noisy (wide IQR, non-monotonic across elevation),
   restrict to clear-sky hours only (`cloud_cover < 25`, the same
   threshold `build_morning_floor()` uses) before trusting it — all-weather
   data at low sun elevations is often too cloud-contaminated to show a
   real shadow pattern, while the same bins filtered to clear-sky hours
   can look completely different.
3. Only add/change a `_OBSERVED_SHADE_OVERRIDES` entry once a bin has
   ≥6 distinct (clear-sky, if filtering) dates and IQR ≤ 0.5 — the
   threshold already used for every other entry in that table.
4. Edit `_OBSERVED_SHADE_OVERRIDES` in `solar_shading_model.py` directly,
   then re-run `python solar_shading_model.py` (or the whole
   `quarterly_update.py`) to pick it up.

**WNW tree profile (`_TREE_SF_PROFILE`) — semi-automated via
`analyse_tree_shadow.py`** (step 8 above, runs every `quarterly_update.py`):
it accumulates hourly WNW-zone evidence into
`data/tree_shadow_calibration.csv` and, for each `el_bin`, compares
`mean_implied` (this run's accumulated evidence) against `model_sf` (the
current `_TREE_SF_PROFILE` value). It always prints the full gap table
either way; it only prints a suggested replacement `_TREE_AZ_BINS`/
`_TREE_EL_ONSET`/`_TREE_SF_PROFILE` block — otherwise it prints "No changes
needed" — **when at least one `el_bin` has both `n ≥ 5` samples AND
`abs(gap) > 0.15`, strictly** (e.g. `gap = -0.149` is a near-miss, not a
qualifying change). More accumulated data each quarter can push a
near-miss bin over the line later, or back under it, without anyone
touching anything.

The profile change is **not applied automatically** — `analyse_tree_shadow.py`
never edits `solar_shading_model.py` itself — but whenever it prints a
suggested block, that block is already sitting in
`docs/charts/quarterly_update.log` (every step's stdout is appended there,
see "What to run" above), so there's no need to re-run anything separately
just to capture it: open the log, copy the printed `_TREE_AZ_BINS`/
`_TREE_EL_ONSET`/`_TREE_SF_PROFILE` block into `solar_shading_model.py` by
hand, then re-run `python solar_shading_model.py` (or the whole
`quarterly_update.py`) to apply it. (Built from the same hourly-actual
data in `solar_15min_history.csv` that every other script in this
pipeline already uses.)

## Last step — sync and flash

**Nothing in this pipeline ever writes directly into
`BatterySolarMonitor/data/`** — that folder is only ever touched by a
deliberate manual step, never silently overwritten by a background
script run. All three files the ESP32 needs land one level above
`BatterySolarMonitorUpdate/` instead, sitting together in one folder for
a single manual copy:

| File | Written by | Changes |
|------|-----------|---------|
| `../site_settings.csv` | You, by hand, whenever site config changes — this pipeline never writes it | Rarely (fixed account/hardware/location config you set once) |
| `../shade_grid.csv` | `quarterly_update.py` step 6, every run | Every run (the refit's whole point) |
| `../morning_floor.csv` | `quarterly_update.py` step 6, every run | Every run |

**Copy all three into `BatterySolarMonitor/data/` by hand** before the
LittleFS upload below — even on a run where `site_settings.csv` itself
didn't change, `shade_grid.csv`/`morning_floor.csv` did, so there's
normally always something to copy after a successful run. The very end
of `quarterly_update.py`'s own log output prints an explicit
"MANUAL STEP REQUIRED" reminder naming all three paths, as a final
prompt not to skip this.

Per this project's standing convention, **compiling and flashing the
ESP32 is done by hand** — sync all three files to the Arduino-IDE copy
(and re-upload the LittleFS data via the Arduino IDE's upload plugin)
when you're ready; this script does not do that automatically.

