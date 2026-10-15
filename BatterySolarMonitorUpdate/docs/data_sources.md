# Data Sources

External APIs the `py_programs/` fetch scripts depend on. The local IQ
Gateway's live endpoints, the hot water controller's TCP protocol, and the
car charger's HTTP API are the ESP32's own direct concern, documented
instead in [`esp32_control_structure.md`](esp32_control_structure.md).

## Contents

1. Open-Meteo Archive API  
    Timezone handling  
2. Enphase Enlighten Cloud API  
    2a. Daily production/consumption lifetime totals  
    2b. 15-minute interval history (per day)  
Summary

---

## 1. Open-Meteo Archive API

**URL:** `https://archive-api.open-meteo.com/v1/archive`
**Auth:** None (public API)
**Timeout:** 60 s
**Used by:** `fetch_solar_history.py`'s `fetch_chunk_utc()`

This is the **archive/history** endpoint,
fetched year-by-year from `FETCH_START = 2023-01-01` (or from the last
stored date + 1, on incremental runs) through yesterday.

**Query parameters:**

| Parameter | Value |
|-----------|-------|
| `latitude` / `longitude` | `fetch_solar_history.py`'s `LAT`/`LON` (site_settings.csv's `latitude`/`longitude` keys) |
| `start_date` / `end_date` | one calendar year at a time, buffered 1 day on each side (see "Timezone handling" below) |
| `hourly` | `global_tilted_irradiance,cloud_cover,direct_radiation,diffuse_radiation` |
| `tilt` | `fetch_solar_history.py`'s `PANEL_TILT` (site_settings.csv's `panel_tilt` key) |
| `azimuth` | `fetch_solar_history.py`'s `PANEL_AZIMUTH` (site_settings.csv's `panel_azimuth` key; Open-Meteo convention: 0=South, -90=East, 90=West) |
| `timezone` | **`GMT+0`, always** — not an IANA name (see "Timezone handling" below) |

### Timezone handling

This endpoint is called with `timezone=GMT+0`, not an IANA name, because
Open-Meteo's archive API applies a single fixed, non-DST-aware UTC offset
to an entire request when given an IANA timezone name — a January-only
request for Sydney (genuinely AEDT/+11) and a July-only request
(correctly AEST/+10) both come back with the same
`utc_offset_seconds=36000` (AEST/+10). Requesting an IANA name would
silently mislabel roughly half of every year's stored timestamps by 1
hour during Australian daylight-saving periods, which
`solar_shading_model.py` uses to compute sun elevation/azimuth for
binning against Enphase's own (correctly timestamped) actual production
— misattributing GTI/production data to the wrong sun-position bin for
the whole shifted stretch each year.

This isn't a one-off quirk of this site's data — it's a known, publicly
reported defect in Open-Meteo's own API (see e.g.
[open-meteo/open-meteo#488](https://github.com/open-meteo/open-meteo/issues/488)
and
[#1764](https://github.com/open-meteo/open-meteo/issues/1764)):
`utc_offset_seconds` is a single value applied to the entire request,
not one that varies per timestamp across a DST transition within the
requested range, for both the archive and forecast APIs alike. One
issue reporter's own suggested workaround — request UTC and have the
client convert to local time itself — is exactly the approach taken
here.

Instead, each returned UTC hourly timestamp is converted to local
wall-clock time in Python, using `POSIX_TIMEZONE` (site_settings.csv's
`posix_timezone` key) via `dateutil.tz.tzstr()` — not `zoneinfo` (IANA
names only, can't parse a POSIX string) or `time.tzset()` (the ESP32's
own mechanism, not available in Windows Python at all). `dateutil` needs
no new install: it's already a transitive dependency of `pandas`, which
every script here already imports. **Pitfall to avoid**: convert each
timestamp with plain `datetime.astimezone()` (via `.to_pydatetime()`
first), never pandas' vectorized `.dt.tz_convert()` or
`pandas.Timestamp.astimezone()` directly — both go through pandas' own
internal fast path, which assumes a pytz/zoneinfo-style tzinfo with
precomputed transition tables. On a `dateutil.tz.tzstr()` object this
doesn't raise cleanly: it fails *inside a destructor* (printed as a
non-fatal "Exception ignored in..." traceback, easy to miss in normal
output) and `pandas.Timestamp.astimezone()` then silently returns the
**original, unconverted** value to the caller — a real, silent wrong-data
bug, not a crash, since no exception actually propagates up to be caught.
Plain stdlib `datetime.astimezone()` uses `tzstr`'s own
`utcoffset()`/`dst()`/`fromutc()` methods directly and is confirmed
correct.

Since Open-Meteo aligns `start_date`/`end_date` to whichever timezone is
requested, asking for `GMT+0` means its day boundaries are UTC days, not
local ones — so each year's request is padded by 1 UTC day on each side
and trimmed back to the exact local year after conversion (a local
calendar day can only be pulled into the adjacent UTC day by at most one
full day, in one direction depending on the site's UTC offset sign, so
one buffer day on each side covers either sign without knowing it in
advance — see `fetch_chunk_utc()`'s own docstring for the full
derivation).

**Response fields used** (all under `hourly`, one array per variable,
same length as `hourly.time`):

| Field | Stored as |
|-------|-----------|
| `time` | `timestamp` |
| `global_tilted_irradiance` | `global_tilted_irradiance` (W/m²) — primary input to the shading model |
| `cloud_cover` | `cloud_cover` (%) — used by `build_morning_floor()`'s clear-day filter (`< 25`) and this pipeline's own clear-sky override calibration |
| `direct_radiation` | `direct_radiation` (W/m²) — used by `diag_gti_recon.py` to reconstruct GTI independently via POA geometry, for cross-checking Open-Meteo's own reported GTI at low sun angles |
| `diffuse_radiation` | `diffuse_radiation` (W/m²) — same use as `direct_radiation` above |

**Output:** `data/solar_history.csv`

---

## 2. Enphase Enlighten Cloud API

**Base URL:** `https://enlighten.enphaseenergy.com`
**Auth:** Session cookie + header token, split across two files (both one
level above `BatterySolarMonitorUpdate/`:-
`../site_settings.csv` is genuine, entirely human-maintained identity/site
CONFIG (never written by any script); `../_session.csv` is pure disposable
run state, regenerated fresh by `auth/enphase_token.py` on every run and
deleted by `quarterly_update.py` once that run finishes.
**Timeout:** 15-20 s
**SSL:** Standard HTTPS

**Fields used by this section's own fetch scripts:**

| Key | Source file | Used for |
|-----|-------------|----------|
| `enlighten_session` | `../_session.csv` | Cookie `_enlighten_4_session` and `e-auth-token` header |
| `enlighten_system_id` | `../site_settings.csv` | Path parameter in all endpoints |

(`../site_settings.csv` also carries `enlighten_email`/`enlighten_password`
— used by `auth/enphase_token.py` to log in — plus `gateway_ip`/
`gateway_serial` and the 8 SITE fields (including `car_charger_ip`/
`hot_water_ip`, used only by the ESP32's `CarChargerFetch.cpp`/
`HotWaterFetch.cpp`, not by this Python pipeline), all copied as-is to
`BatterySolarMonitor/data/site_settings.csv` by hand; see
`auth/enphase_token.py`'s own `load_config()` for the full field set --
`save_session()` is unrelated, it only ever writes `../_session.csv`. Both
copies are 13 fields; `open_meteo_timezone` is not one of them, since
Open-Meteo is called with a fixed `GMT+0` offset instead of an IANA name
(see the "Timezone handling" section above) — the ESP32's own
`SiteSettings.h`/`SolarForecastFetch.cpp` match, see
`esp32_control_structure.md` §1/§1a).

**Request headers:**
```
Cookie: _enlighten_4_session={enlighten_session}
e-auth-token: {enlighten_session}
Accept: application/json
User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)
X-Requested-With: XMLHttpRequest
```

Refreshed by `auth/enphase_token.py` — see that script's own docstring and
[`quarterly_update_process.md`](quarterly_update_process.md). All 13
fields in `../site_settings.csv` (5 Enphase identity fields, alongside 8
site/location fields — see
[`quarterly_update_process.md`](quarterly_update_process.md)) are copied
into `BatterySolarMonitor/data/site_settings.csv` **by hand** whenever the
file changes, where the ESP32's `EnphaseAuth.h`/`SiteSettings.h` load
them at boot as fixed config. `enlighten_session` (`../_session.csv`) is
**not** part of this file at all — the ESP32 performs its own Enlighten
login (`EnlightenLoginFetch.h`) and persists the result into its own
separate `auth_token.csv` on LittleFS, so a PC-side copy of the live
session would be stale within a day. See
[`esp32_control_structure.md`](esp32_control_structure.md) §1 for the
full identity-vs-live-token split.

### 2a. Daily production/consumption lifetime totals

**Endpoints:**
- `GET /systems/{system_id}/energy_lifetime`
- `GET /systems/{system_id}/consumption_lifetime`

**Used by:** `fetch_enlighten_daily.py`'s `_fetch_lifetime()`

| Response field | Used for |
|----------------|----------|
| `start_date` | First date in the `production`/`consumption` array |
| `production[i]` | Daily Wh, indexed from `start_date` — stored as `production_kwh`, the regression target `solar_shading_model.py` fits against |
| `consumption[i]` | Daily Wh, indexed from `start_date` — stored as `consumption_kwh`; **for info only**, no reader anywhere in this pipeline |

Only complete days up to and including yesterday are kept (today's partial
day is always skipped). **Output:** `data/enphase_daily.csv`.

### 2b. 15-minute interval history (per day)

**Endpoint:** `GET /systems/{system_id}/power_time_series?all_production_sources=true&days=1&date=YYYY-MM-DD`
**Used by:** `fetch_consumption_15min.py`'s `fetch_day()` (called once per
historical date needed)

| Response field | Used for |
|----------------|----------|
| `consumption[i]` | True home load (W) per 15-min slot — Enphase computes this from all meters, battery effects already excluded. Stored as `data/consumption_15min_history.csv`; **for info only**, no reader anywhere in this pipeline |
| `acb.acb_c_ts[i]` | Battery charge power (W) per slot, always ≥ 0. Stored as `data/battery_charge_15min_history.csv`; **for info only** as its own file, but also feeds the `grid_net_w` power-balance derivation below |
| `acb.acb_d_ts[i]` | Battery discharge power (W) per slot, always ≥ 0. **Not stored under its own column at all** — used only transiently, within this same fetch, to compute `grid_net_w` below |
| `production.meter[i]` | Solar production (W) per slot. Stored as `data/solar_15min_history.csv` — the one field here with real downstream consumers (`solar_shading_model.py`, every `diag_*.py` script, `plot_gti_vs_model.py`, `analyse_tree_shadow.py`) |

Normally 96 slots (15-min × 24h); see `fetch_consumption_15min.py`'s own
docstring for the two DST transition days (92/100 slots), a since-changed
Enlighten API quirk (97 raw slots per day, the extra one a next-day
boundary point truncated away before the DST checks run), and
how they're all handled. `grid_net_w` is derived, not returned directly — see
that script's power-balance comment. **For info only**: nothing in this
pipeline (or the ESP32 firmware) reads `grid_net`/
`data/grid_15min_history.csv` back — it's written every run for manual
inspection but has no current consumer.

**Output:** `data/consumption_15min_history.csv`,
`data/solar_15min_history.csv`, `data/battery_charge_15min_history.csv`,
`data/grid_15min_history.csv` (all four from the same API call per day) —
of these four, only `solar_15min_history.csv` has any actual reader.

---

## Summary

| Source | Type | Host | Auth | Used by |
|--------|------|------|------|---------|
| Open-Meteo Archive | HTTPS REST | archive-api.open-meteo.com | None | `fetch_solar_history.py` |
| Enlighten Cloud | HTTPS REST | enlighten.enphaseenergy.com | Session cookie + header | `fetch_enlighten_daily.py`, `fetch_consumption_15min.py` |
| Enlighten login | HTTPS REST | enlighten.enphaseenergy.com | Email/password (`../site_settings.csv`) | `auth/enphase_token.py` |
