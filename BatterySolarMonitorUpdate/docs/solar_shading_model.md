# Solar Shading Model

## Contents

1. Purpose  
2. Known Shading Obstructions  
3. Input Data Sources  
    3a. Open-Meteo Archive API — `data/solar_history.csv`  
    3b. Enphase Daily Production — `data/enphase_daily.csv`  
    3c. Solar 15-min History — `data/solar_15min_history.csv`  
4. Solar Position Calculation  
5. Sun-Position Bins  
6. Shade Factor Regression  
7. Post-Processing Pipeline  
8. Tree Shadow Correction  
9. Easterly Shading (summer, tree to the east)  
10. Morning Floor Correction — `data/morning_floor.csv`  
11. Output — `data/calibrated_solar_hourly_v2.csv`  
12. How the Output Is Actually Used  
13. Model Accuracy

---

## Purpose

Produces `data/shade_grid.csv` — the per-sun-position shade factor lookup
table `BatterySolarMonitor/ShadeFactorTable.cpp` loads at boot and applies
to its own live Open-Meteo forecast (see
[`esp32_control_structure.md`](esp32_control_structure.md) §7) — and
`data/morning_floor.csv` — the `(month, hour) → clear-sky floor kW` lookup
`MorningFloorTable.cpp` loads the same way, applied by `SolarForecast.h`
as an empirical floor on the shading-corrected estimate (the same
"floor_effective" logic this script's own `apply_shading()` uses, below).
These two files are the only outputs of this pipeline the ESP32 actually
consumes.

As a byproduct of the same run, this also produces
`data/calibrated_solar_hourly_v2.csv` — hourly solar production estimates
corrected for site-specific shading. The ESP32 never reads this file (it
does its own live forecasting).

**Why this file exists, given `shade_grid.csv` already is the real
output**: `shade_grid.csv` is only a lookup table (shade factor per sun
position) — it can't by itself answer "how accurate is this model?". To
measure that, the finalized shade factors have to actually be *applied*,
hour by hour, against the full historical GTI record and compared to real
production — that application is exactly what this file is. It's the
required intermediate between "we solved some shade factors" and "here's
how well they predict reality," and it earns its keep twice: within the
*same* run, `main()` passes the in-memory result straight into
`make_charts()`/`write_report()` to produce
`docs/charts/solar_shading_model.png` and `docs/solar_shading_stats.md`
(the RMSE/MAE/Bias/R² figures in [Model Accuracy](#model-accuracy)
below come from here); and on disk, two separate later processes read it
back without re-running the whole regression: `plot_gti_vs_model.py`
(step 7 of `quarterly_update.py`, see
[`quarterly_update_process.md`](quarterly_update_process.md)) to build
`docs/charts/gti_vs_model.png`, and `diag_afternoon.py` for its own
ad-hoc per-hour comparison.

**When it runs:** manually, roughly every 3 months — `python
quarterly_update.py` runs the full pipeline (fetch Open-Meteo history →
fetch Enphase production → fetch 15-min history → refit model → copy
`shade_grid.csv` to `BatterySolarMonitor/data/`). See
[`quarterly_update_process.md`](quarterly_update_process.md) for the full
runbook. A daily cadence isn't needed here since this lookup table only needs refreshing
occasionally, as seasons and tree growth shift the shading pattern.


---

## Known Shading Obstructions

Two separate tree obstructions affect this site:

- **Trees ~10 m WNW–N of the house.** These shade the panels through the
  afternoon in most seasons (Stage 3, geometry-based correction, az 280-319°)
  and through the middle of the day in winter, when the low winter sun tracks
  close to due north (Stage 4 override entries, az 320-350°). This is the
  best-calibrated obstruction — see
  [Tree Shadow Correction](#tree-shadow-correction) below. This is the
  **northerly winter shading**.
- **A tree to the east of the house.** This shades the panels on summer
  mornings, when sunrise happens furthest south (SE) and the sun's low
  early-morning arc swings through due east before climbing clear of the
  tree. A provisional geometry-based correction still
  covers most of this zone, but 7 bins (az 80-100°) now have real per-bin
  empirical overrides instead, see
  [Easterly Shading](#easterly-shading-summer-tree-to-the-east) below for
  the calibration and its caveats.

---

## Input Data Sources

### 1. Open-Meteo Archive API — `data/solar_history.csv`

Fetched by `fetch_solar_history.py` from `https://archive-api.open-meteo.com/v1/archive`.

| Parameter | Value |
|-----------|-------|
| Latitude | site latitude |
| Longitude | site longitude |
| Panel tilt | 30° |
| Panel azimuth | -120° (Open-Meteo convention: 0=South, -90=East, 90=West) |
| Timezone | `GMT+0` (not the site's IANA name — see [Timezone handling](data_sources.md#1-open-meteo-archive-api) in `data_sources.md`) |
| Start date | 2023-01-01 |

Hourly variables stored per row:

| Column | Description |
|--------|-------------|
| `timestamp` | Local time (includes DST) |
| `global_tilted_irradiance` | GTI on panel plane (W/m²) — primary input to model |
| `cloud_cover` | Cloud cover (%) |
| `direct_radiation` | Direct horizontal irradiance (W/m²) — used by `diag_gti_recon.py`, not the main regression |
| `diffuse_radiation` | Diffuse horizontal irradiance (W/m²) — same use as `direct_radiation` above |

### 2. Enphase Daily Production — `data/enphase_daily.csv`

Actual daily kWh from the Enphase Enlighten `energy_lifetime`/
`consumption_lifetime` APIs. One row per day with columns `date`,
`production_kwh`, and `consumption_kwh`. Only `production_kwh` is used, as
the regression target (ground truth) — `consumption_kwh` is fetched and
stored but has no reader anywhere in this pipeline (for info only).

### 3. Solar 15-min History — `data/solar_15min_history.csv`

Fetched by `fetch_consumption_15min.py` from Enlighten's `power_time_series`
endpoint (`solar_w` field of the same per-day API call that also produces
`consumption_15min_history.csv`/`battery_charge_15min_history.csv`/
`grid_15min_history.csv` — see [`data_sources.md`](data_sources.md)'s
§2b for those three; this is the only one of the four with any actual
reader). Provides per-hour actual production values as
supplementary regression observations, directly constraining individual
sun-position bins that daily totals alone can't isolate (see
[Shade Factor Regression](#shade-factor-regression) below).

96 columns (`slot_0`..`slot_95`, 15-min kW readings) per day, fetched
incrementally forward by that script's own `main()`, back to 2025-09-01
(confirmed that Enlighten's `power_time_series` endpoint still returns
real historical data well before that — not limited by an API retention
window). One date, 2025-10-05, is the AEDT daylight-saving spring-forward
date (a genuine 23-hour local day, 92 slots not 96) and needs a one-off
remap (raw index → local wall-clock slot, +4 slots from the 2am→3am jump
onward) rather than the normal fetch path, which hard-rejects any day
that isn't exactly 96 slots.

Separately, Enlighten itself changed this endpoint's response at some
point: every date queried (including long-settled historical ones,
re-fetched fresh) now comes back with **97** raw slots, not 96 — index 96
is a boundary point at 24:00:00 (the very start of the *next* day, a
small/zero value consistent with an overnight instant, confirmed not to
equal that next day's own index-0 value), not a real 97th interval of the
requested day. Left unhandled, this would hit the same "unexpected
length" rejection as a genuine data anomaly and silently skip every day
fetched since the API changed. `fetch_day()` truncates to the first 96
slots before the DST-length checks above, so a 97-slot response is
treated as a normal 96-slot day with one harmless trailing point dropped,
while a still-anomalous length (92/100 on a non-DST date, or anything
else) continues to be rejected as a real data gap.

---

## Solar Position Calculation

Each hourly row in `solar_history.csv` is mapped to a sun position using the
Spencer (1971) equations implemented in `solar_position()`.

**Steps:**

1. Determine UTC offset from local timezone (accounts for DST)
2. Convert local time to UTC, then to solar time:
   ```
   solar_hour = hour_UTC + LON/15 + EoT/60
   ```
   where EoT is the Equation of Time in minutes (accounts for Earth's elliptical orbit)
3. Hour angle: `HA = 15° × (solar_hour − 12)` (0 = solar noon, positive = afternoon)
4. Solar declination from day-of-year using Spencer Fourier series
5. Solar elevation: `sin(el) = sin(lat)·sin(decl) + cos(lat)·cos(decl)·cos(HA)`
6. Solar azimuth from North clockwise (0=N, 90=E, 180=S, 270=W); afternoon hours reflected to west

---

## Sun-Position Bins

The sky is divided into a discrete grid:

| Dimension | Bin size |
|-----------|----------|
| Azimuth | 10° |
| Elevation | 5° |

Each hour is assigned `az_bin = floor(azimuth/10)×10` and `el_bin = floor(elevation/5)×5`,
combined into a unique `pos_bin = az_bin×100 + el_bin`. Only hours with GTI > 10 W/m²
are used in the regression.

---

## Shade Factor Regression

For each sun-position bin a `shade_factor` is solved such that:

```
actual_kwh_day ≈ Σ_bins ( shade_factor[bin] × Σ_hours_in_bin( SYSTEM_KW × GTI/1000 ) )
```

where `SYSTEM_KW = 7.6`.

### Method — bounded least squares with Tikhonov regularisation

**Design matrix X** (rows = days, columns = bins):
`X[day, bin]` = sum of `base_kw` (= 7.6 × GTI/1000) for all hours on that day
in that bin.

**Target vector y**: Enphase actual kWh for each day.

**Supplementary hourly rows** (if `solar_15min_history.csv` available):
Each hour contributes one row `X[hour, bin] = base_kw`, `y[hour] = actual_kw`.
This directly constrains individual bins that may not dominate any single day's total.

**Recency weighting** — every daily and hourly row is
weighted by `0.5 ^ (age_years / RECENCY_HALF_LIFE_YEARS)`, where `age_years`
is measured back from the most recent training date and
`RECENCY_HALF_LIFE_YEARS = 3.0` by default. Applied as `sqrt(weight)` row
scaling (standard weighted least squares) before the regularisation rows are
appended, so recent years pull the fit harder than old ones — the working
assumption is that tree growth since the 2023 data was collected means older
rows understate current shading. With a 3-year half-life the oldest rows
(2023) currently carry ~0.47× the weight of the most recent day. Bin energy
for the regularisation prior below is computed from this recency-weighted
matrix too, so a bin with only old evidence is treated as poorly-constrained
even if its raw (un-decayed) sample count looks fine. Set
`RECENCY_HALF_LIFE_YEARS = None` in `solar_shading_model.py` to disable and
weight all years equally. Fit-quality reporting (`RMSE`/`MAE`/`R²` printed by
`solve_shading()`) and the `total_predicted_kwh` energy totals used
elsewhere still use the *unweighted* matrix, since those describe actual
physical kWh, not fit-time importance.

**Regularisation** — pulls poorly-constrained bins toward a prior of 0.85:
```
lambda[bin] = (median_bin_energy / bin_energy) × 1.5
```
Low-energy bins (few observations) are pulled strongly toward 0.85; well-observed
bins are left free.

**Bounds**: shade factors constrained to [0.0, 1.3]
- 0.0 = fully shaded / sun never reaches panels there
- 0.85 = typical system derating (inverter, temperature, wiring, soiling)
- 1.0 = Open-Meteo prediction is exact
- 1.3 = OM underestimates (albedo or reflection boost — generous cap)

Solved with `scipy.optimize.lsq_linear` using the BVLS method.

---

## Post-Processing Pipeline

After the NNLS regression, five post-processing stages are applied before the
shade factors are written to `calibrated_solar_hourly_v2.csv`:

### Stage 1 — Spatial neighbourhood smoothing

`smooth_shade_factors(shade_dict, results, iterations=2)`

For each (az, el) bin, takes an energy-weighted average with its 8 grid
neighbours (±10° in azimuth, ±5° in elevation).  The central bin gets 4×
weight; handles azimuth wrap-around at 0/360°.

Raises isolated low-value bins that were suppressed by coincidental cloud
cover in the training data, without smearing genuinely coherent shading
patterns.  2 iterations is enough to connect sparse bin neighbours while
avoiding over-smoothing.

### Stage 2 — Sun-path temporal dip removal

`smooth_along_sun_path(shade_dict, iterations=10)`

Generates 73 representative dates (every 5 days through the year) to trace
the sun's daily arc across the (az, el) grid.  For each day's hourly bin
sequence, any local minimum is raised toward the mean of the path-predecessor
and path-successor bins.  10 iterations until convergence.

**Why this stage is needed:** in summer the noon sun sweeps rapidly across the
sky — the 10am and 12pm hours may be 3-5 grid bins apart even though they
feel temporally adjacent.  Spatial smoothing (Stage 1) cannot bridge that gap,
leaving isolated dip bins caused by cloud-correlated training artefacts.
Sun-path smoothing walks the actual daily arc and catches dips that spatial
smoothing misses.

### Stage 3 — Tree shadow correction (WNW zone)

`apply_tree_shadow_correction(shade_dict)`

Overrides shade factors in the WNW tree shadow zone (az 280-319°, el 0-49°)
with geometry-based estimates calibrated from actual hourly production data.
This replaces NNLS-fitted values which are systematically too high in this
zone because the regression is trained on daily totals — morning
under-prediction and afternoon over-prediction cancel in the daily sum,
hiding the hourly shape error.

See the [Tree Shadow Correction](#tree-shadow-correction) section below.

### Stage 3b — East tree shadow correction (provisional)

`apply_east_tree_shadow_correction(shade_dict)`

Overrides shade factors in az 80-140°, el < 30° with a 2-point geometry
profile (el≤15°: SF=0.157, el 15-25°: SF=0.497; no correction at el≥30°)
derived from a season-pooled regression against real summer daily totals
— see [Easterly Shading](#easterly-shading-summer-tree-to-the-east) for
the full calibration and its caveats. Same rationale as Stage 3 (NNLS
leaves this zone near 1.0 because it can't isolate the effect from daily
totals). Built without any hourly ground truth, so still provisional —
Stage 4 (below) unconditionally overrides 7 of these bins with real
per-bin values where hourly evidence exists for them, so this stage's
output for those specific bins is superseded; it still determines the
rest of the zone.

### Stage 4 — Empirical override table (`_OBSERVED_SHADE_OVERRIDES`)

`apply_observed_overrides(shade_dict)`

After all smoothing and the tree shadow correction, per-bin shade factor
overrides from `_OBSERVED_SHADE_OVERRIDES` are applied.  Each entry
unconditionally replaces the NNLS (or tree-corrected) shade factor for a
specific `(az_bin, el_bin)` key.

**Why this stage is needed:** the NNLS regression minimises daily-total error
across all seasons.  Some bins are visited only in winter (e.g. az=320-350°,
el=25-35°) when the sun is low in the northern sky.  Spatial smoothing cannot
bridge the multi-bin gap between summer and winter sun paths, so NNLS values
for those bins are poorly constrained and can be wrong by 0.1-0.4.  Empirical
overrides derived from clear-day hourly observations fix these bins directly.

**Trade-off — clear-day vs cloudy-day accuracy:**
Override values are calibrated from clear-day hours (high GTI, clear implied
SF).  On cloudy days diffuse light wraps around obstructions, making the
effective shade factor higher than a clear-day value implies.  Winter
calibration therefore reduces over-prediction on clear afternoons at the cost
of slight under-prediction on cloudy winter days.

**This is where `shade_grid.csv` is actually written**: `write_shade_grid(shade_dict)`
exports the lookup table immediately after Stage 4 above finishes, using
whatever `shade_dict` is at that point — this is the real, ESP32-consumed
output (see [Purpose](#purpose) above). `data/calibrated_solar_hourly_v2.csv`
(see [Output](#output--datacalibrated_solar_hourly_v2csv) below) is a
**separate, independent byproduct**, computed *afterward* by re-applying
this same already-finalized `shade_dict` across the full hourly history —
it is not an input `shade_grid.csv` depends on, and `shade_grid.csv` is
not derived from it either; both are downstream of the one shared
`shade_dict`, just used for two different purposes (a compact per-bin
lookup table vs. a full hour-by-hour reconstruction for charts/stats/
inspection).

#### Calibration workflow

Three diagnostic scripts guide the process:

| Script | Purpose |
|--------|---------|
| `diag_morning_overrides.py` | Lists bins where empirical mean SF diverges from model by ≥ 0.08 and IQR ≤ 0.50 (stable signal) |
| `diag_afternoon.py` | Per-hour forecast vs actual for last 7 days — shows which hours over- or under-predict |
| `diag_bin_dates.py` | Per-bin statistics (mean_sf, n_obs, date range) from 15-min history joined with sun-position data |

**Steps:**

1. Run `python diag_morning_overrides.py` to find candidate bins.
2. Cross-check with `python diag_afternoon.py` to confirm systematic over/under by hour.
3. Run `python diag_bin_dates.py` and filter to candidate bins to get reliable mean_sf
   values (aim for n ≥ 10, IQR ≤ 0.40 to avoid cloud-contaminated measurements).
4. Add entries to `_OBSERVED_SHADE_OVERRIDES` in `solar_shading_model.py`.
5. Rerun `python solar_shading_model.py` and check seasonal bias in `docs/solar_shading_stats.md`.

#### Current override table (32 entries)

Grouped by the obstruction each zone is attributed to.

**Northerly winter shading — az 320-350° (northward tree, winter afternoon/near-noon):**

| az_bin | el_bin | SF | Notes |
|-------:|-------:|:---:|-------|
| 340 | 30 | 0.44 | n=67 May-Jul |
| 320 | 25 | 0.31 | n=35 May-Jun winter 2pm |
| 320 | 30 | 0.24 | n=14 Apr-May |
| 320 | 35 | 0.30 | n=6 Apr |
| 330 | 35 | 0.61 | n=19 Apr-May |
| 330 | 40 | 0.75 | n=14 Apr |
| 330 | 45 | 0.80 | n=13 Apr |
| 340 | 35 | 0.61 | n=20 Jul-Aug (NNLS had this near 1.0, uncorrected) |
| 350 | 30 | 0.81 | n=46 Apr-Jul winter near-noon |
| 350 | 35 | 0.89 | n=23 Jun |
| 350 | 40 | 0.85 | n=16 Apr-May |
| 350 | 50 | 0.75 | n=11 Apr |

Checked against the full 167-day (Mar-Sep) 15-min-actuals window, now that
the fit uses [recency weighting](#method--bounded-least-squares-with-tikhonov-regularisation).
Most bins match (gap ≤ 0.07); `(340,35)` has 20 dates of clean signal
(IQR 0.57-0.69) sitting under an uncorrected NNLS value near 1.0, so it's
included as an override.

**Morning — az 10-70° (mostly OM under-prediction, not shading; see note below):**

| az_bin | el_bin | SF | Notes |
|-------:|-------:|:---:|-------|
| 10 | 30 | 0.97 | n=64 Apr-Jul NNE morning |
| 20 | 45 | 0.82 | n=11 Apr-May |
| 30 | 25 | 1.20 | n=31 Apr-Jul (large gap +0.20) |
| 30 | 30 | 1.05 | n=23 Apr-Jul |
| 30 | 35 | 1.02 | n=18 Apr-May |
| 40 | 15 | 1.35 | n=28 Apr-Jul (wide IQR — cloud-contaminated, watch) |
| 40 | 25 | 1.09 | n=16 Apr-May (large gap +0.25) |
| 40 | 40 | 0.99 | n=14 Apr |
| 50 | 15 | 1.25 | n=15 Jun low-el ENE, OM under-predicts |
| 50 | 30 | 1.03 | n=23 Apr-May |
| 60 | 15 | 0.87 | n=13 Apr-May |
| 60 | 20 | 1.08 | n=15 Apr-May (large gap +0.41) |
| 70 | 5 | 1.65 | n=16 Apr-May sunrise (NNLS-capped) |

Almost all of the morning-zone entries are **above** 1.0 — the NNLS solver
under-predicts these bins, it is not shading. All of this calibration data is
from Apr-Jul (autumn/winter) sunrise azimuths (NNE-ENE); the sun does not
pass through az 10-70° at low elevation in summer at this latitude (summer
sunrise is much further south, around SE), so this table says nothing about
the east-tree summer shading described below — that obstruction sits at
higher azimuth (az 80-110°, due-east-ish) and is only in view during
Oct-Feb. See [Easterly Shading](#easterly-shading-summer-tree-to-the-east).

**When to add new overrides:** watch for bins where `diag_morning_overrides.py`
shows a gap ≥ 0.10 with n ≥ 10 and IQR ≤ 0.40.  Winter bins (May-Jun, az
320-350°, el 25-40°) are the most likely candidates as sun coverage there is
seasonal and the NNLS is under-constrained.

**East tree (summer mornings, az 80-100°):**

| az_bin | el_bin | SF | Notes |
|-------:|-------:|:---:|-------|
| 80 | 20 | 0.70 | n=9 Oct |
| 90 | 25 | 0.77 | n=20 Nov-Dec |
| 90 | 30 | 0.99 | n=8 Jan |
| 100 | 5 | 1.52 | n=6 Jan-Feb — OM underestimates, not shaded |
| 100 | 10 | 0.58 | n=27 Nov-Jan |
| 100 | 15 | 0.54 | n=6 Nov-Dec |
| 100 | 25 | 0.83 | n=10 Dec |

These replace `apply_east_tree_shadow_correction()`'s provisional geometry
estimate for just these 7 bins (see
[Easterly Shading](#easterly-shading-summer-tree-to-the-east) below for the
full calibration method — clear-sky filtering was needed to get a usable
signal here, unlike the rest of this table). Coverage outside these 7 bins
(az 80° el 10/15/25/30, az 90° el 5/10/15/20, all of az 110-130°, and
el > 30° generally) is still under the 6-distinct-clear-sky-dates bar this
table uses elsewhere — those bins still fall back to the provisional
profile.

---

## Tree Shadow Correction

### Geometry

Trees are approximately 10 m from the house in the WNW direction.  From
autumn actual data the shading onset is ~33° elevation, implying an effective
tree height of ~11 m above the panel plane.

At higher elevations (el ≈ 40-45°, az ≈ 310-320°) the tree crown still clips
the sun in April afternoon at ~2pm (el_bin=40, az_bin=310), confirming the
crown extends above the geometric onset angle for those azimuths.

### Calibration data (`data/tree_shadow_calibration.csv`)

`analyse_tree_shadow.py` runs daily after each model refit and writes one row
per qualifying hourly observation to `data/tree_shadow_calibration.csv`.
Qualifying hours: sun in WNW zone (az 280-319°, el 0-49°) with actual
production recorded and GTI > 10 W/m².

Columns: `date, hour, az_bin, el_bin, sun_elevation, sun_azimuth, gti_wm2,
base_kw, actual_kw, shade_factor, implied_sf`

where `implied_sf = actual_kw / base_kw` is the empirical shade factor for
that hour and position.

### Current profile (`_TREE_SF_PROFILE`)

Calibrated from qualifying hourly observations in
`data/tree_shadow_calibration.csv`. `analyse_tree_shadow.py` reruns this
comparison every `quarterly_update.py` (step 8) — its output is captured
live in `docs/charts/quarterly_update.log`, not a separate log file.
`model_sf` below is the FINAL post-smoothing/post-correction value for
that bin from `shade_grid.csv`, not the raw `_TREE_SF_PROFILE` input.
This depends on `solar_history.csv`'s sun-position timestamps being
correctly DST-labeled — Open-Meteo's archive API can't be trusted to do
that itself when given an IANA timezone name, so it's requested in UTC
and converted locally instead; see
[`data_sources.md`](data_sources.md)'s "Timezone handling" section for
the full story:

| el_bin | n | mean_implied | model_sf | gap | notes |
|-------:|--:|:------------:|:--------:|:---:|-------|
| 0 | 61 | 0.010 | 0.100 | -0.090 | extrapolated minimum, matches well |
| 5 | 104 | 0.085 | 0.097 | -0.011 | 5pm low sun; high variance (max 0.694); well matched |
| 10 | 66 | 0.115 | 0.140 | -0.025 | deep shadow |
| 15 | 113 | 0.206 | 0.190 | +0.016 | largest sample, well matched |
| 20 | 111 | 0.194 | 0.240 | -0.046 | consistent |
| 25 | 65 | 0.235 | 0.130 | +0.105 | directly observed |
| 30 | 70 | 0.296 | 0.360 | -0.064 | Apr 15h, az≈302-310° |
| 35 | 68 | 0.412 | 0.397 | +0.015 | Mar 16h, az≈300°; well matched |
| 40 | 78 | 0.508 | 0.550 | -0.042 | Apr 14h, az≈317°; conservative |
| 45 | 59 | 0.641 | 0.500 | +0.141 | larger sample |
| 50 | — | — | 1.00 | — | onset threshold — no correction above |

`gap = mean_implied - model_sf`. Every bin is within `|gap| <= 0.15`, so
`analyse_tree_shadow.py` itself prints "No changes needed" for this table
— it only ever proposes an update once `n >= 5` **and** `abs(gap) > 0.15`,
strictly (see [`quarterly_update_process.md`](quarterly_update_process.md)'s
"WNW tree profile" section for the exact rule and how a qualifying
suggestion gets logged).

`el_bin=5`'s own `_TREE_SF_PROFILE` entry in `solar_shading_model.py` is
manually set to 0.081, on the judgment that n=104 is solid evidence
despite a near-miss gap. The bin's *final* `model_sf` (0.097, after
smoothing/corrections downstream of the raw `_TREE_SF_PROFILE` input)
sits close to the accumulated evidence (gap -0.011), so no further
hand-editing is needed.

High-variance bins (max implied_sf > 1.0, e.g. el_bin=5,20 — see the full
min/max columns `analyse_tree_shadow.py` itself prints, trimmed from this
table) are affected by partial cloud cover — clear-sky values are lower, so
profile values are intentionally conservative (set below the mean to avoid
over-prediction on clear days).

### How `tree_shadow_calibration.csv` is used

`data/tree_shadow_calibration.csv` is used **only** by `analyse_tree_shadow.py`
as an accumulating log of empirical evidence.  It has no effect on forecasts —
nothing in the daily pipeline reads it to produce predictions.

Its sole purpose is to let you see whether `_TREE_SF_PROFILE` needs updating
as more seasonal data arrives.

### Profile refinement workflow

`analyse_tree_shadow.py` runs automatically as step 8 of every
`quarterly_update.py`. It builds its hourly-actual data from
`solar_15min_history.csv`, the same source every other script in this
pipeline already uses. It appends to `tree_shadow_calibration.csv` and prints a
gap table comparing accumulated evidence against `_TREE_SF_PROFILE`. See
[`quarterly_update_process.md`](quarterly_update_process.md) for how to
read its output; it never edits `solar_shading_model.py` itself — copy a
suggested block in by hand.

After the summary table, the script automatically compares the calibration data
against the current model constants and prints either:

- **No changes needed** — all el_bins with n ≥ 5 have |gap| ≤ 0.15.
- **Suggested updates** — complete replacement constants for `_TREE_AZ_BINS`,
  `_TREE_EL_ONSET`, and `_TREE_SF_PROFILE`, ready to copy-paste into
  `solar_shading_model.py`.  Only el_bins with **n ≥ 5 and |gap| > 0.15** are
  changed; others retain their current value.  If the highest qualifying el_bin
  is within one step (5°) of the current `_TREE_EL_ONSET`, the onset is raised
  by 5°.

**To update the profile when changes are suggested:**

1. Open `docs/charts/quarterly_update.log` and find the latest
   `analyse_tree_shadow` section, or run `python analyse_tree_shadow.py`
   directly.
2. Copy the printed `_TREE_AZ_BINS`, `_TREE_EL_ONSET`, and `_TREE_SF_PROFILE`
   block and paste it into `solar_shading_model.py`, replacing the existing
   constants.
3. Run `python solar_shading_model.py` to refit and regenerate
   `calibrated_solar_hourly_v2.csv` and `solar_shading_model.png`.
4. Run `python analyse_tree_shadow.py` to confirm gaps are now small.
5. Update the calibration table in this doc and the tasklog.

**Note on high-variance bins:** bins where `max_implied > 1.0` include
cloud-affected days where actual irradiance was lower than GTI, making the
implied shade factor appear higher than it really is.  If the suggested value
for such a bin seems too high, use a value slightly below the printed
`mean_implied`.

**Future refinements to watch for:**

- **el_bin=40-45**: more data from summer afternoons (Dec-Feb) when the sun
  reaches 2pm az≈315-320° at higher elevations.
- **az 320-330°** (not yet in `_TREE_AZ_BINS`): may show shading in summer
  at ~4pm when the sun moves further north-west; add those bins if evidence
  accumulates.

---

## Easterly Shading (summer, tree to the east)

A tree to the east of the house shades the panels on summer mornings:
sunrise happens furthest south (SE) around the December solstice at this
latitude, and the sun's low early-morning arc sweeps through due-east
azimuths (≈80-140°) before climbing high enough to clear the tree. This is a
**separate obstruction** from the WNW-N trees above.

Without hourly ground truth for this zone (see
[Why there isn't hourly calibration data for the rest of this zone](#why-there-isnt-hourly-calibration-data-for-the-rest-of-this-zone)
below), the whole zone relies on a provisional, coarser correction
(`apply_east_tree_shadow_correction`, Stage 3b) except where real hourly
coverage exists. `solar_15min_history.csv` covers az 80-100° (Oct-Feb)
back to 2025-09-01, giving part of this zone real hourly coverage — see
[Real per-bin calibration](#real-per-bin-calibration) below. 7 bins use
real per-bin overrides (`_OBSERVED_SHADE_OVERRIDES`,
[Stage 4](#stage-4--empirical-override-table-_observed_shade_overrides));
the rest of the zone (az 80° el 10/15/25/30, az 90° el 5/10/15/20, all of
az 110-130°, and el > 30° generally) still falls back to the provisional
geometry profile described below.

### Why there isn't hourly calibration data for the rest of this zone

`data/solar_15min_history.csv`, the hourly-actual source used to calibrate
the WNW tree profile and the Stage 4 overrides, is fetched incrementally
forward from `fetch_consumption_15min.py`'s own first run. Within a
window spanning only autumn into winter, the sun's low-elevation morning
azimuth never reaches past az_bin=80 — sunrise azimuth swings northeast
(toward az≈64° at the June solstice) rather than through the due-east
zone this tree occupies, so that window alone can never accumulate real
evidence for this zone no matter how long it runs, since the season this
tree affects (Oct-Feb) simply falls outside it. This is a "hasn't happened
yet" problem, not a stale-data problem, and it's addressed not by waiting
for the season to come around again but by confirming that Enlighten's
`power_time_series` endpoint still returns real historical data well
before the fetch script's first run, and backfilling directly to
2025-09-01 — see [Solar 15-min History](#3-solar-15-min-history--datasolar_15min_historycsv)
above. That backfilled window does cover real Oct-Feb mornings, unlike a
live-forward-only window.

### Real per-bin calibration

Joining `solar_15min_history.csv` against sun-position bins (same method
as `diag_bin_dates.py`, applied to the east zone) shows **noisy,
non-monotonic** implied shade factors across az 80-130° when using all
weather — plausibly cloud/weather variance dominating at these low-sun,
low-GTI hours rather than a clean geometric shadow signal. Restricting to
clear-sky hours only (`cloud_cover < 25`, the same threshold
`build_morning_floor()` already uses elsewhere) cleans this up
substantially: az=100° in particular then shows a plausible monotonic
tree-shadow gradient (el 10→15→20→25°: 0.58 → 0.54 → 0.62 → 0.83 — deeper
shade low, clearing higher) that the cloud-contaminated all-weather
version doesn't show at all.

7 bins meet this table's usual acceptance bar (≥6 distinct clear-sky
dates, IQR ≤ 0.5) and are added directly to `_OBSERVED_SHADE_OVERRIDES`
— see the [east-tree entries in Stage 4](#current-override-table-32-entries)
above for the values. Coverage outside those 7 bins is still too thin
post-clear-sky-filter (<6 dates) and continues to rely on the provisional
profile below.

### How the provisional correction (still covering the rest of the zone) was calibrated

Since fine-grained per-bin data doesn't exist for most of this zone, the
fallback correction instead comes from a coarser but real signal:
a 2-parameter regression against **daily** production totals
(`enphase_daily.csv`, which does span real summers back to 2023), pooling
days by distance from the **Dec-21 solstice** (folding Nov together with
Jan-Feb, so 4 partial calendar-year summers combine into one 454-day sample)
rather than matching literal calendar dates. Each day's unshaded predicted
energy (`base_kw = 7.6 × GTI/1000`) was split into "east zone" (az 80-140°)
vs "rest of day", further split by elevation, and solved against actual
production:

| Elevation | Shade factor | n |
|---|:---:|--:|
| el ≤ 15° | 0.157 | 454 |
| el 15-25° | 0.497 | 454 |
| el > 25° | *(no correction — no evidence)* | — |

This produces a plausible tree-shaped gradient — deep shadow low, tapering
higher — matching the qualitative shape of the WNW `_TREE_SF_PROFILE`. It
also lines up with what raw per-bin NNLS output showed before this
correction: those bins sat at 1.0-1.07 (i.e. treated as essentially
unshaded), which the fine-grained per-bin regression apparently can't detect
because the east zone is too thin a slice of any single day's energy
(~7% on average) to move an under-constrained 214-bin fit — but pooling all
of that zone's energy into one parameter gives it enough statistical leverage
to show up clearly.

**The caveat that keeps this "provisional":** re-running the same fit
separately per summer season is unstable — SF ranged from **0.13 (2025) to
0.65 (2025-26)** across the four seasons in the data, versus IQR ≤ 0.2 for
the north-tree bins calibrated on real hourly data. Whole-day totals are a
noisy way to isolate a ~7%-of-energy effect; ordinary cloud variability
between summers can swing a 2-parameter fit by that much. Treat the 0.157 /
0.497 values as a reasonable central estimate, not a tightly-pinned one —
this is why the 7 bins with real clear-sky hourly evidence were moved to
`_OBSERVED_SHADE_OVERRIDES` instead of staying on this profile.

### Recommended follow-up

As spring/summer data accumulates in `solar_15min_history.csv`, re-run
`diag_bin_dates.py` restricted to az 80-140°, el < 30° (clear-sky
filtered) to extend real per-bin coverage to the remaining thin bins —
especially az 110-130° and el > 30°, which currently have no usable
clear-sky evidence. Retire `_EAST_TREE_SF_PROFILE` entirely once enough
of the zone has real `_OBSERVED_SHADE_OVERRIDES` entries to cover it.

This correction (both the provisional profile and the real overrides)
measurably reduces the model's summer over-prediction — see
[Model Accuracy](#model-accuracy) below.

---

## Morning Floor Correction — `data/morning_floor.csv`

### Purpose and need

Open-Meteo's `global_tilted_irradiance` is systematically too low at sun
elevations below ~10°, often by a factor of 5-10x — real panels see direct
beam sunlight that Open-Meteo's model dramatically attenuates near the
horizon. This is a **different problem** from every correction above: the
tree-shadow and empirical-override corrections all work by adjusting a
*shade factor* that multiplies GTI, but a multiplicative factor can only
ever scale an input that's already roughly right — it can't fix an input
that's wrong by 5-10x to begin with, no matter how large the factor is
allowed to run. Instead of scaling around it, `build_morning_floor()`
derives an empirical **lower bound** on production directly from observed
history, and the model raises its shade-corrected estimate up to that
floor whenever the estimate would otherwise sit below it.

### Calculation (`build_morning_floor()`)

For each `(month, hour)` bucket:

1. Join `solar_15min_history.csv` (hourly actual kW, averaged from that
   hour's 4 15-min slots) with `solar_history.csv`'s own `cloud_cover` for
   the matching date/hour.
2. Keep only hours where `actual_kw > 0.05` (filters out genuine
   night/near-zero readings).
3. If **at least 3 days** in that bucket had `cloud_cover < 25` (the same
   clear-sky threshold used elsewhere in this pipeline), `floor_kw_clear`
   is the **mean** `actual_kw` across just those clear days.
4. Otherwise (fewer than 3 clear-sky days seen so far for that
   month/hour), falls back to the **median** `actual_kw` across *all* days
   in the bucket, clear or not — a coarser, cloud-contaminated estimate,
   but better than no floor at all for a bucket that hasn't accumulated
   enough clear-sky evidence yet.

**Output:** `data/morning_floor.csv`, columns `month`, `hour`,
`floor_kw_clear`, `n_clear` (number of clear-sky days behind the mean, 0
if the median fallback was used instead), `n_total` (total days seen for
that bucket, clear or not).

### Applied at runtime (`apply_shading()`, and identically by the ESP32's
own `SolarForecast.h::estimatedKw()`)

Both the PC-side model and the ESP32's own live forecast apply the exact
same formula, in the same order:

```
cloud_factor    = clip(1 - cloud_cover/100 * 0.6, 0.4, 1.0)
floor_effective = floor_kw_clear * cloud_factor
est_kw_shading  = max(est_kw_shading, floor_effective)   # only while sun is up
```

The floor is scaled **down** by that hour's own cloud cover (1.0× on a
clear hour, down to 0.4× fully overcast) so a genuinely cloudy morning
doesn't get artificially floored up to a clear-sky value — and it's a
`max()`, so it only ever **raises** the shade-corrected estimate, never
lowers it, and only while the sun is actually up (`sun_elevation > 0` on
the PC side, `elevationDeg > 0.0` on the ESP32). A bucket with no
`morning_floor.csv` entry at all (`floorKw` reads as 0 on the ESP32, or
the lookup simply misses on the PC side) applies no floor, same as if
this correction didn't exist.

**Sanity-checking the combined result**: `diag_tomorrow.py` fetches
tomorrow's live Open-Meteo forecast (the same GMT+0/`posix_timezone`
approach as `fetch_solar_history.py`'s own `fetch_chunk_utc()` — see
[`data_sources.md`](data_sources.md)'s "Timezone handling" section) and
prints each hour's GTI, shade factor, and floor contribution side by
side, so a suspicious-looking forecast on the live Status/chart display
can be reproduced and inspected on the PC side one input at a time,
rather than only ever seen as the ESP32's own already-combined output.

---

## Output — `data/calibrated_solar_hourly_v2.csv`

**Not the same output as `shade_grid.csv`, and not derived from it or
vice versa** — `shade_grid.csv` is already written earlier in the run
(see the note at the end of [Stage 4](#stage-4--empirical-override-table-_observed_shade_overrides)
above), directly from the finalized per-bin `shade_dict`. This file is a
separate byproduct of the *same* `shade_dict`, computed afterward by
`apply_shading()` re-applying it hour-by-hour across the full history in
`solar_history.csv` (plus the morning-floor correction) — a full
reconstruction useful for charts, stats, and inspection, not a step
`shade_grid.csv` needs or feeds into.

One row per hour across the full history. Columns:

| Column | Description |
|--------|-------------|
| `timestamp` | Local time (includes DST) |
| `gti_wm2` | GTI on panel plane (W/m²) from Open-Meteo |
| `sun_elevation` | Solar elevation angle (degrees) |
| `sun_azimuth` | Solar azimuth from North clockwise (degrees) |
| `shade_factor` | Solved shade factor for this hour's sun-position bin (0–1.3); unmapped bins default to 0.85 |
| `est_kw_old` | Naive estimate: `7.6 × GTI/1000 × 0.80` |
| `est_kw_shading` | Calibrated estimate: `7.6 × GTI/1000 × shade_factor`, clipped to [0, 7.6] kW |

---

## How the Output Is Actually Used

**On the ESP32 (the live consumer today)**: `ShadeFactorTable.cpp` doesn't
read this CSV — it reads `data/shade_grid.csv` (the per-bin lookup table
`write_shade_grid()` exports, copied to `BatterySolarMonitor/data/` by
`quarterly_update.py`), loaded once at boot into a fixed-size array. For
each hour of its own live Open-Meteo forecast, `SolarForecast.h` computes
the sun position (`SunPosition.cpp`, the on-device equivalent of this
file's own `solar_position()`) and applies the matching shade factor:

```
kw = SYSTEM_KW * gtiWm2 / 1000.0 * shadeFactorForSunPosition(azimuthDeg, elevationDeg)
```

Same formula as `apply_shading()` above — see
[`esp32_control_structure.md`](esp32_control_structure.md) §7 for the full
detail.

**On the PC side**: `plot_gti_vs_model.py` (step 7 of `quarterly_update.py`)
reads `calibrated_solar_hourly_v2.csv` directly (`gti_wm2`, `shade_factor`,
`est_kw_shading`) to build `docs/charts/gti_vs_model.png` — see
[`quarterly_update_process.md`](quarterly_update_process.md)'s "How to
read the output" section. `diag_afternoon.py` reads the same file for its
own ad-hoc per-hour forecast-vs-actual comparison.

---

## Model Accuracy

Fit vs the naive uniform 0.80 derating, computed against 1204 days of
daily Enphase totals (214 sun-position bins, 203 with significant
energy):

| Model | RMSE (kWh/day) | MAE (kWh/day) | Bias (kWh/day) | R² |
|-------|:--------------:|:-------------:|:--------------:|:---:|
| Naive uniform 0.80 | 6.25 | 4.92 | +2.37 | 0.793 |
| **Shading model (final)** | **5.99** | **4.44** | **-1.08** | **0.810** |

Bias is mean(actual − predicted): positive = model under-predicts, negative = over-predicts.
See `docs/solar_shading_stats.md` (auto-generated on each run) for the current
figures — the numbers above are a snapshot.

**Seasonal breakdown (shading model):**

| Season | Days | RMSE | MAE | Bias |
|--------|-----:|:----:|:---:|:----:|
| Summer (Nov-Feb) | 361 | 7.77 | 5.91 | -2.56 |
| Equinox (Mar-Apr / Sep-Oct) | 391 | 5.51 | 4.22 | -0.53 |
| Winter (May-Aug) | 452 | 4.59 | 3.45 | -0.37 |

The shading model beats the naive baseline on every measure (RMSE 5.99 vs
6.25, R² 0.810 vs 0.793). This relies on `solar_15min_history.csv` giving
the NNLS regression supplementary hourly rows, not daily totals alone,
plus the real east-tree overrides described above.

Summer bias sits at -2.56 kWh/day — the dominant driver of overall RMSE
— because the east-tree zone still only has real per-bin evidence for
part of it (see [Easterly Shading](#easterly-shading-summer-tree-to-the-east)
above); summer remains the largest remaining error source while the
east-tree correction is still only partially promoted from provisional to
real overrides. The equinox bias is small negative (-0.53).

Winter bias sits at -0.37 (small over-prediction): the northerly winter
overrides (az 320-350°, el 25-35°, see
[Stage 4](#stage-4--empirical-override-table-_observed_shade_overrides))
correct the clear-day-calibration gap that would otherwise cause winter
under-prediction on cloudy days.

- **RMSE** (Root Mean Square Error) — square root of the average squared difference
  between predicted and actual daily kWh. Penalises large errors more heavily than
  small ones. Lower is better.
- **MAE** (Mean Absolute Error) — average absolute difference between predicted and
  actual daily kWh. Easier to interpret directly: on a typical day the model is off
  by this many kWh. Lower is better.
- **R²** (coefficient of determination) — fraction of the day-to-day variance in
  actual production that the model explains. 1.0 = perfect; 0.0 = no better than
  predicting the mean every day. Higher is better.

See `docs/solar_shading_stats.md` (auto-generated on each run) for the
current figures and `analysis/charts/solar_shading_model.png` for the shading
map, azimuth/elevation profiles, residuals, and scatter plots.
