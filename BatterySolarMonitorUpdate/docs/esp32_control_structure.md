# ESP32 Control Structure (BatterySolarMonitor)

## Purpose

This documents how `BatterySolarMonitor` (the FireBeetle 2 ESP32-E firmware)
does live solar forecasting, car-charger/hot-water load control, and status
display entirely on-device.
Two things the ESP32 still relies on the PC pipeline for, both loaded from
LittleFS data files at boot rather than hardcoded in source: the
shading-model lookup table (§7) and its Enphase credentials (§1) — see
[`quarterly_update_process.md`](quarterly_update_process.md) for that side.

All source referenced below lives in `BatterySolarMonitor/`.

---

## Contents

1. Credentials — `EnphaseAuth.h`  
    1a. Site config — `SiteSettings.h`  
    1b. Self-refresh — `EnlightenLoginFetch.h`  
2. One shared background task for every internet-facing poll  
3. Car charger / hot water: Fetch + Task + Instances pattern  
4. Ownership tracking — `LoadControlState.h`  
5. The control decision — `LoadControlTask.cpp`  
    5a. V2H trip recovery  
    5b. Blocking a start near the V2H window  
6. Status display — `LoadStatusFormat.h/.cpp`  
7. Live forecast + shading lookup  
8. Average consumption forecast + battery SOC projection  
9. Car charge history — `CarChargeHistory.h/.cpp`  
10. Buzzer alarm — `BuzzerControl.h/.cpp`  
11. Daily reboot — `DailyReboot.h/.cpp`  
12. Debug output + Log Tail — `TimestampStream.h`, `DebugLogBuffer.h/.cpp`  
13. Enlighten history backfill — `EnlightenHistoryFetch.h/.cpp`  
14. Car charger / hot water wire protocols  
15. Discharge-to-grid button — `DischargeToGridState.h`/`DischargeToGridFetch.h`/`DischargeToGridTask.h`
16. Hot Day — `HotDayState.h`/`ChargeFromGridState.h`/`ChargeFromGridFetch.h`/`HotDayTask.h`


---

## 1. Credentials — `EnphaseAuth.h`

Two Enphase credentials the ESP32 needs — the local IQ Gateway's JWT
(JSON Web Token — a signed, self-contained access token; here it's just
an opaque bearer credential the gateway issues and later verifies, not
something this project ever decodes or inspects itself) + IP
(`GatewayFetch.cpp`, live battery/solar/consumption readings) and the
Enlighten cloud session cookie + system ID (`EnlightenHistoryFetch.cpp`,
backfills today's already-elapsed history once near boot, then again
every 15 minutes for the rest of the day — §13 — since the local
gateway only ever gives "right now") — are **different credentials** for
**different APIs**, and expire on different clocks (the gateway JWT lasts
months; the Enlighten session cookie only ~14 days).

Hardcoding these as `static const char*` constants would mean updating
either one by manually pasting a fresh value into the ESP32 source and
recompiling + reflashing. Instead,
`EnphaseAuth.h`/`EnphaseAuth.cpp` load values from LittleFS
(`loadEnphaseAuth()`, called once from `setup()`, same pattern as
`ShadeFactorTable.cpp`'s `loadShadeFactorTable()`).

Two files, split by owner:

- **`/site_settings.csv`** — IDENTITY (`gateway_ip`, `enlighten_system_id`,
  `enlighten_email`, `enlighten_password`, `gateway_serial`) — fixed
  account/hardware config that can't be derived on-device. **PC-owned**:
  lives at `../site_settings.csv` (one level above
  `BatterySolarMonitorUpdate/`, kept outside `BatterySolarMonitor/data/`
  since it holds the plaintext Enlighten account password among other
  identity fields) as a genuine, entirely human-maintained config file --
  nothing in the PC-side pipeline writes to it. Copied into
  `BatterySolarMonitor/data/site_settings.csv` **by hand** whenever it's
  edited, re-upload at the next
  sync+flash, not a source edit. `loadEnphaseAuth()` requires all 5 of
  these to report success. This same file also carries 8 further fixed per-site fields
  (latitude, longitude, panel tilt/azimuth, system kW, timezone, car
  charger/hot water LAN IPs) — see §1a below; `EnphaseAuth.h`/
  `SiteSettings.h` are two independent loaders sharing the one file, each
  reading only its own keys.
- **`/auth_token.csv`** — LIVE TOKENS (`gateway_jwt`, `enlighten_session`)
  — short-lived credentials that expire and need refreshing.
  **Device-owned** — see Self-refresh below. `quarterly_update.py` does
  **not** write this file at all (a PC-side copy would be stale within a
  day); empty, or the file not existing yet, on a freshly-seeded device is
  normal, not a load failure. Split into its own file specifically because
  it's rewritten — `saveAuthToken()`
  only ever touches these 2 fields.

`gatewayIp()`/`gatewayJwt()`/`enlightenSession()`/`enlightenSystemId()`
return the loaded values regardless of which file they came from — callers
don't need to know or care about the split. **Keep `site_settings.csv` out
of any public repo/screen-share** — `enlightenPassword()` is a real
account password. `auth_token.csv` is just as sensitive in principle
(`gatewayJwt()` grants local API access to the whole solar/battery/
consumption system), but the same caution doesn't apply the same way:
it only ever exists on the device's own LittleFS filesystem, never on
the PC side at all, so there's no repo or screen-share it could actually
leak through.

### 1a. Site config — `SiteSettings.h`

Fixed per-site physical/location/network config — coordinates, panel
install geometry, nameplate capacity, timezone, and the LAN IPs of the
two other local controllers this device talks to directly — loads from
`/site_settings.csv` (the same file §1's identity fields live in) via
`SiteSettings.cpp`'s `loadSiteConfig()`, called once from `setup()` —
same LittleFS-data-file pattern as `ShadeFactorTable.h`/`EnphaseAuth.h`,
rather than scattering these as hardcoded constants across
`SolarForecastFetch.cpp`/`SunPosition.cpp` (`LAT`/`LON`),
`SolarForecast.h` (`SYSTEM_KW`), `ntpSupport.cpp`'s weak
`get_ntpSupport_DefaultTZ()` (POSIX TZ string), `CarChargerFetch.cpp`/
`HotWaterFetch.cpp` (their own `HOSTNAME` literals), and bare unnamed
literals in `SolarForecastFetch.cpp`'s URI string (tilt/azimuth).
Changing a site constant is a data-file re-upload, not a source edit +
recompile + reflash.

Eight keys, all required (`loadSiteConfig()` returns false, and logs the
full expected key list, if any are short): `latitude`, `longitude`,
`panel_tilt`, `panel_azimuth`, `system_kw`, `posix_timezone`,
`car_charger_ip`, `hot_water_ip`. `posix_timezone` (a POSIX TZ string)
feeds `SiteSettings.cpp`'s own strong override of `ntpSupport.cpp`'s weak
`get_ntpSupport_DefaultTZ()` declaration. `car_charger_ip`/`hot_water_ip` feed
`CarChargerFetch.cpp`'s/`HotWaterFetch.cpp`'s own `siteCarChargerIp()`/
`siteHotWaterIp()` calls — the pfodWeb Designer-generated
`pfodMainMenu.cpp`'s "EV Settings" link and the Enphase system ID in its
"Enphase Settings" link are also rebuilt from this same loaded config at
boot (`initDynamicMenuLabels()`, `pfodMainMenuApp.cpp`), rather than
keeping the Designer's own generated literal — see that function's own
comment.

**`open_meteo_timezone` (an IANA name) is not one of this file's keys** —
`SolarForecastFetch.cpp` does not feed an IANA name to Open-Meteo's
`timezone=` query parameter, because Open-Meteo's archive API applies a
single fixed, non-DST-aware UTC offset to an entire IANA-timezone
request (see `BatterySolarMonitorUpdate/docs/data_sources.md`'s
"Timezone handling" section for the full story — the same defect, same
fix, on both the PC and the device side). Instead, `SolarForecastFetch.cpp`
requests `timezone=UTC` and converts each returned hourly timestamp to
local wall-clock time itself, via `localtime_r()` — the device's global
`TZ` is already set correctly by `ntpSupport.cpp`'s `ESP_setTZ()` (via
`sitePosixTimezone()`, above), so no extra `setenv()`/`tzset()` call is
needed there. There's also no POSIX-TZ↔IANA-name conversion table on
this device, so a single POSIX TZ string (`posix_timezone`) serves both
this device's own clock and its Open-Meteo forecast query, with nothing
else needed. See §7 below for the fetch/parsing details.

The daily weather forecast (`temperature_2m_max`, `temperature_2m_min`,
`precipitation_sum`) rides along on the SAME request `SolarForecastFetch.cpp`
already makes for hourly solar GTI — no second HTTPS connection and one less poll on `InternetPollTask.cpp`'s tick order
(§2). `WeatherForecastInstances.h` (the `todayWeatherForecast`/
`tomorrowWeatherForecast` model instances, read by `Dwg_StatusApp.cpp`)
is populated by `SolarForecastTask.cpp`'s `checkSolarForecastPoll()`
under `lockSolarForecast()`/`unlockSolarForecast()` (`SolarForecastTask.h`).
`WeatherForecast.h`'s rainfall field is a single deterministic figure
(Open-Meteo's `precipitation_sum`) — `Dwg_StatusApp.cpp` shows it as
`~X.Xmm` (the `~` flagging it as an estimate, not a measured amount),
`---` for a genuine 0mm forecast.

`loadSiteConfig()` must run before `resetDefaultTZstr()` and before
anything that could reach `SolarForecastFetch.cpp`/`SunPosition.cpp` —
`BatterySolarMonitor.ino`'s `setup()` calls it right after
`loadEnphaseAuth()`, before `setupWiFi()`.

### 1b. Self-refresh — `EnlightenLoginFetch.h`

The device keeps `gatewayJwt()`/`enlightenSession()` current itself —
entirely device-managed from the first successful login onward;
`/site_settings.csv` (§1, human-maintained on the PC side) only ever
seeds the identity fields that make this possible. `checkEnlightenLoginPoll()` runs on the shared background task
(§2); each attempt (after a short startup-uptime settling window on first
call) does one of two things depending on what's already in
`/auth_token.csv`:

- **No existing token** (file missing/empty — a fresh device, or one that
  just had its LittleFS data re-uploaded): builds one immediately via a
  full 2-step login. POST email/password to
  `enlighten.enphaseenergy.com/login/login.json` for a session_id + the
  `_enlighten_4_session` cookie, then POST that session_id to
  `entrez.enphaseenergy.com/tokens` for a fresh JWT.
- **Existing token present**: TESTED first, not blindly replaced.
  `checkGatewayTokenValid()`/`checkEnlightenTokenValid()`
  (`EnlightenLoginFetch.cpp`) each make one lightweight authenticated
  request (`GET /ivp/meters` on the local gateway; `GET
  /systems/{id}/energy_lifetime` on Enlighten — chosen because neither
  needs a `date=` parameter, so neither depends on NTP having synced yet)
  and classify the result three ways: confirmed valid (200), confirmed
  invalid (401), or inconclusive (anything else — connect failure,
  timeout, a different error). Only a **confirmed 401 on either check**
  triggers a full rebuild (same login flow as above); an inconclusive
  result is treated the same as valid, deliberately — a transient
  connectivity blip during the test must never itself trigger an
  unnecessary relogin, or "test first" would provide no benefit over
  always rebuilding.

Either way, once a valid token is confirmed (tested-valid, or freshly
built), that's it for this boot — `DailyReboot.h`'s daily reboot naturally
re-triggers the next check tomorrow, comfortably inside the session
cookie's ~14-day freshness window. If a rebuild is needed and FAILS, it's
retried every `RETRY_INTERVAL_MS` (5 min) — the same interval as this
project's various `POLL_INTERVAL_MS` constants — **indefinitely, until one
succeeds**. There is no "give up until tomorrow": the still-cached
`gatewayJwt()`/`enlightenSession()` from the last successful login remain
valid and keep working the whole time (even if untested this cycle), so a
failed rebuild costs nothing beyond staying on the old (still good, or at
worst unconfirmed) credential a bit longer.

**Missing credentials** (`enlightenEmail()`/`enlightenPassword()` empty —
shouldn't happen, since `loadEnphaseAuth()` requires them, but could
follow a hand-edited/truncated `/site_settings.csv`): logged once as a
configuration fault and NOT retried, since a retry can't fix a missing
credential. Checked before the existing-token logic above, so it takes
priority over either branch.

**Real-usage feedback — `notifyAuthTokenRejected()`**: the once-per-boot
check only looks at the token's state at that one moment. If it was
genuinely still valid then but expires or gets revoked sometime later the
same day, nothing would otherwise notice until tomorrow's reboot.
`GatewayFetch.cpp` (all three of its endpoints) and
`EnlightenHistoryFetch.cpp` (`httpGetFromCandidates()`) each call
`notifyAuthTokenRejected()` whenever a REAL (non-validation) request comes
back with a confirmed 401. That resets the "done for this boot" state and
re-arms the attempt timer to fire immediately — since `checkGatewayPoll()`/
`checkEnlightenHistoryPoll()` both run earlier than `checkEnlightenLoginPoll()`
in `InternetPollTask.cpp`'s tick order (§2), a 401 detected this tick can
trigger a full re-check-and-rebuild before that same tick finishes. Guards
against notify calls piling up mid-outage: it's a no-op once a re-check is
already pending, so a run of repeated 401s from one ongoing problem (e.g.
`GatewayPollTask.cpp`'s own 10s retry cadence while `latestBatterySocPercent`
is stuck at its -1 sentinel) doesn't keep resetting `RETRY_INTERVAL_MS`'s
5-minute backoff once a rebuild attempt is already in flight.

On success, `setGatewayJwt()`/`setEnlightenSession()` (`EnphaseAuth.h`)
update the in-RAM values and `saveAuthToken()` persists them back to
`/auth_token.csv` on LittleFS — `site_settings.csv` is never touched by
this — without that persistence step, a self-refreshed token would only
last until the next midnight reboot wiped RAM.

DNS for both hosts is resolved once at boot (`resolveEnlightenLoginHosts()`,
called from `setup()` alongside `resolveSolarForecastHost()` — **main
thread only, never from a background task**. Resolving a hostname (as
opposed to connecting via an already-cached IP-literal) can call into
ESP-IDF's `dns_clear_cache()`, which is unsafe from any task other than
the one holding the lwIP/TCPIP core lock — a confirmed match to
[espressif/esp-idf#14287](https://github.com/espressif/esp-idf/issues/14287),
and the actual root cause (confirmed via a symbolicated crash backtrace)
of a real crash this project hit, which always originated from a solar
forecast fetch running on the background task. Resolving once, here, on
the main thread avoids that path entirely; every later connection then
uses the cached IP (an IP-literal string), which short-circuits before
ever reaching `dns_clear_cache()`. Every other DNS-resolving function in
this project (`resolveSolarForecastHost()`, `resolveEnlightenHosts()`,
§13) follows the same rule for the same reason).

`enlightenHostIP()` exposes the resulting cached IP for
`enlighten.enphaseenergy.com` to other files — added so
`DischargeToGridFetch.cpp` (§15) could reuse it instead of resolving the
exact same hostname a third time.

The one specific risk flagged in the source itself:
ESP32's `HTTPClient::collectHeaders()`/`header()` may only capture a
single value if the login response sends more than one `Set-Cookie`
header (the real `_enlighten_4_session` cookie might not be the one kept)
— `loginStep()` logs a specific warning if the cookie can't be found,
which is the first thing to check if self-refresh never succeeds.

---

## 2. One shared background task for every internet-facing poll

`InternetPollTask.cpp` runs a single FreeRTOS task that ticks every job in
turn, once a second:

```
checkGatewayPoll() -> checkSolarForecastPoll()
  -> checkEnlightenHistoryPoll() -> checkAvgConsumptionPoll()
  -> checkEnlightenLoginPoll() -> checkCarChargerPoll() -> checkHotWaterPoll()
  -> checkLoadControlPoll() -> checkDischargeToGridPoll() -> checkDischargeToGridLogic()
```

`checkSolarForecastPoll()` fetches the daily weather forecast (temp
max/min, rainfall) in the same Open-Meteo request as the hourly solar
forecast.

Each job still owns its own "is it due yet" timer and interval — this file
just ticks all of them. **Why one shared task instead of one per job**: an
earlier bug (`GatewayFetch.cpp`'s `/ivp/ensemble/power` handler) showed the
local gateway's embedded HTTPS server breaks if two `WiFiClientSecure`
connections are open at once, even from different tasks. Running every poll
from one task guarantees only one internet-facing connection is ever open
at a time — no cross-task overlap is possible. Cost: a slow poll delays the
ones queued behind it in the same round, but every poll is already bounded
(~13s worst case), so a worst-case ~50s serialized delay (every job
coinciding on the same tick) is a rare, one-off event, not a sustained
problem. The normal idle cadence for every job is 5 minutes or longer —
`CarChargerTask.cpp`'s/`HotWaterTask.cpp`'s own post-toggle/V2H recheck
timers (§3, as short as 20s) are the exception, running far more often
for a short while after a toggle — but those are themselves "soon after,"
not hard-deadline, promises, so an occasional worst-case serialized delay
stacking on top of one is still tolerable at this timescale.

`checkLoadControlPoll()` runs **last**, deliberately — it reads the state
`checkCarChargerPoll()`/`checkHotWaterPoll()` (just above it in the same
tick) just published, so it's always acting on this cycle's freshest fetch,
not a stale one from before.

---

## 3. Car charger / hot water: Fetch + Task + Instances pattern

Both loads follow the same 3-file pattern, independently:

| Role | Car charger | Hot water |
|---|---|---|
| Raw device I/O | `CarChargerFetch.h/.cpp` (plain HTTP to the charger) | `HotWaterFetch.h/.cpp` (raw TCP pfod protocol to the controller) |
| Poll scheduling + publishing | `CarChargerTask.h/.cpp` | `HotWaterTask.h/.cpp` |
| Shared state (mutex-guarded) | `CarChargerInstances.h/.cpp` | `HotWaterInstances.h/.cpp` |

Each `*Task.cpp` polls its device on its own `pfodDelay` timer,
`POLL_INTERVAL_MS = 5 min`, and publishes the result into the matching
`*Instances` globals under a lock (`lockCarChargerState()`/
`unlockCarChargerState()`, and the hot-water equivalents) — the only way
any other file touches this state.

**Post-toggle recheck (20s) — refreshing the STATE, not re-running the
DECISION**: `requestCarChargerRecheck()`/`requestHotWaterRecheck()` re-arm
that device's own poll timer, here in `*Task.cpp`, to `RECHECK_DELAY_MS =
20s` instead of the normal 5 minutes. `LoadControlTask.cpp` calls these
right after every successful toggle, so the *real* device state gets
confirmed within that 20s for both devices — comfortably past either
one's own worst-case transition time (a few seconds for hot water, up to
~8s for the car charger. The car V2H turn on has it own timer, see below) — not left standing on the optimistic
post-toggle guess (§5) for up to 5 minutes.

This is a **completely different timer from `LoadControlTask.cpp`'s own
`EARLY_RECHECK_MS` (30s, §5)** — easy to conflate since both fire "soon
after a toggle", but they do different jobs on different schedules:

| | This 20s timer | `LoadControlTask`'s 30s timer |
|---|---|---|
| Lives in | `CarChargerTask.cpp`/`HotWaterTask.cpp` | `LoadControlTask.cpp` |
| Job | Re-fetch the device and overwrite the optimistic guess with the real state | Re-run the SOC (State of Charge — battery %) hysteresis decision logic (§5) |
| Runs | Once, 20s after a toggle | Every `EARLY_RECHECK_MS` until a normal-cadence cycle passes with nothing to defer |

Because `InternetPollTask.cpp` (§2) always runs `checkCarChargerPoll()`/
`checkHotWaterPoll()` before `checkLoadControlPoll()` in the same 1s tick,
and 30s > 20s, the control decision at t=30s is *guaranteed* to see the
real state the 20s timer already fetched ten seconds earlier — never the
optimistic placeholder — **for hot water, unconditionally** (its poll timer
only ever serves this one reason). This ordering is deliberate, not
incidental; see §4's note on why it also made a separate startup-grace
timer unnecessary.

For the car charger specifically, that "20s always lands by t=30s" guarantee
has one rare exception since the priority arbitration below was added: if a
higher-priority V2H recheck/schedule-follow-up (30s/60s) happens to already
be counting down in `CarChargerTask.cpp` the instant this 20s request is
made, the request is deferred rather than fired at t=20 — so `LoadControlTask`'s
own t=30s decision could in that coincidence still see the pre-toggle
placeholder. See the priority table just below for why that's the correct
trade-off rather than a bug.

**V2H schedule-boundary follow-up**: the charger's own `v2h_auto` on/off
schedule (`EV_Charger_V2H_switch/switchSettings.cpp`'s `handleAuto()`, see
§5a) switches entirely on that OTHER device, on its own clock —
`CarChargerTask.cpp`'s plain 5-minute poll has no awareness of it. A poll
landing seconds before the boundary (e.g. `14:01:03` for
`v2h_time_on=14:01`) would otherwise read the pre-flip state, with the
next poll a full 5 minutes away — the device log (and the Status display)
would show the stale `v2h=off` for most of that window even though the
schedule had already flipped it. `checkCarChargerPoll()` tracks
`lastV2hScheduleCheckMin` (local minutes-since-midnight as of the last poll
with a synced clock) and, each successful fetch where `v2hAutoActive` is
true, checks whether `v2hOnTimeMinutes`/`v2hOffTimeMinutes` was crossed
since that last check (`crossedBoundary()`, wraparound-safe at midnight). If
so, it re-arms its own poll timer for `V2H_SCHEDULE_FOLLOWUP_MS` (**60s**)
instead of the normal 5 minutes, so one extra fetch lands promptly after the
boundary; the next poll after that reverts to the normal cadence
automatically (that same boundary won't "cross" again).

That follow-up fetch only refreshes `CarChargerInstances.h` — on its own it
does nothing to make `LoadControlTask.cpp`'s independent `pollTimer`/
`Status:` log line reflect it any sooner (that file's poll cycle runs on a
completely separate schedule, §2/§5). See "Sync with `LoadControlTask.cpp`"
below for how that gap is closed.

**Sync with `LoadControlTask.cpp`**: `CarChargerTask.cpp`'s poll cadence is
not phase-aligned with `LoadControlTask.cpp`'s own independent
`pollTimer`, which without this sync could let `carCheckStart()` make a
start decision against a `car.v2hActive` reading that's nearly a full
poll-interval stale, right at the charger's own V2H schedule boundary
(confirmed against a real captured device log). The fix:
`checkCarChargerPoll()` calls `requestLoadControlPollNow()`
(`LoadControlTask.h`) **unconditionally after EVERY successful fetch** —
not just the schedule-boundary follow-up or the V2H-recovery recheck
(§5a). That call forces `checkLoadControlPoll()`'s **full** 4-phase
decision cycle to run within about a second, using this fetch's fresh
data. Every trigger does the same, correct thing rather than a lighter,
status-only refresh for some cases — an accelerated poll must not sneak
in an extra, off-cadence run of `carCheckStart()`'s V2H recovery retry
logic, and §5b's schedule-aware start guard is what makes it safe for
every trigger to take the full path.

Safe to call this on every successful fetch, not just special ones: each
call corresponds to genuinely fresh, distinct data (`InternetPollTask.cpp`'s
tick order, §2, guarantees `checkLoadControlPoll()` runs after
`checkCarChargerPoll()`/`checkHotWaterPoll()` in the same tick), and the 4
decision phases are idempotent against repeated fresh evaluation — e.g.
`carCheckStart()`'s V2H recovery retry clears `carV2hRecoveryPending()`
**before** sending its toggle, so a second call immediately after never
re-sends it. A **failed** fetch triggers nothing — `LoadControlTask.cpp`'s
own `V2H_RECOVERY_RECHECK_MS`/`EARLY_RECHECK_MS` reschedules (§5/§5a) remain
as the fallback for a persistently-unreachable charger, and
`LoadControlTask.cpp`'s own timer keeps running regardless either way (see
below).

**Why `LoadControlTask.cpp` still needs its own timer at all**, given this
sync: `checkCarChargerPoll()` is not its only input.
`checkLoadControlPoll()` also depends on fresh Gateway (SOC/house load) and
hot-water readings, and neither `GatewayPollTask.cpp` nor `HotWaterTask.cpp`
triggers it the same way — so a SOC (State of Charge — battery %) crossing a hysteresis threshold, or a
hot-water reading changing, still needs its own regular cadence to ever get
evaluated. More importantly, if the car charger goes unreachable and this
sync were the *only* trigger, `LoadControlTask.cpp` would never run again
until it came back — silently stopping HW auto-control too, an unrelated
failure domain. The car-fetch sync is a "wake up sooner when there's a good
reason to" layer on top of an otherwise still-necessary regular heartbeat,
not a replacement for it.

**Priority arbitration between the 3 car-charger recheck reasons**:
`CarChargerTask.cpp`'s `pollTimer` is one shared timer serving 4 different
reasons to wake up early. Without priority arbitration ("last caller
wins", i.e. `pollTimer.start(newDelay)` on every request unconditionally),
a V2H schedule-boundary follow-up armed for 60s (above) could be cut
short to 20s by an unrelated plain charger toggle
(`requestCarChargerRecheck()`) happening moments later, firing the
follow-up fetch before the schedule's own flip had necessarily even
landed — a real (if rare) problem. Each reason's delay is a *minimum
settle time*, not an arbitrary deadline, so a later request for a
SHORTER delay must never pre-empt an earlier, still-relevant request for
a LONGER one — but any of the 3 real reasons should freely override the
plain 5-minute idle default, since that's not a "reason" at all, just
what happens when nothing special is pending. `currentRecheckPriority`
ranks them accordingly:

| Priority | Reason | Delay | Needs that long because |
|---|---|---|---|
| 0 `RECHECK_NONE` | Idle default | `POLL_INTERVAL_MS` (5min) | Nothing special pending — always overridable |
| 1 `RECHECK_CHARGER` | Plain charger toggle | `RECHECK_DELAY_MS` (20s) | Relay settling (~8s) |
| 2 `RECHECK_V2H` | V2H recovery toggle-**on** | `V2H_RECHECK_DELAY_MS` (30s) | Turning V2H **on** settles slowly (~16s+: ~2 relay-settling cycles plus a few seconds for AC to actually start) — turning it off is almost instant |
| 3 `RECHECK_SCHEDULE_FOLLOWUP` | Schedule-boundary crossing | `V2H_SCHEDULE_FOLLOWUP_MS` (60s) | Margin past the charger's own minute-boundary flip |

`armPollTimerIfHigherPriority(delayMs, priority)` — used by
`requestCarChargerRecheck()`/`requestCarChargerV2hRecheck()` — only applies
a request whose priority is `>=` `currentRecheckPriority`; a lower-priority
request while a higher one is already counting down is silently dropped
(the wait already in flight keeps running). An *equal*-priority re-request
(e.g. a second V2H toggle before the first's 30s has elapsed) still restarts
the wait fresh from now — a legitimate debounce, not a pre-emption, since
the second toggle genuinely needs its own full settle time measured from
when IT happened. The authoritative end-of-poll decision inside
`checkCarChargerPoll()` itself (which reason applies to the *next* poll)
bypasses this gate entirely and sets both `pollTimer` and
`currentRecheckPriority` unconditionally — by the time a poll actually
runs, whatever reason armed it has been serviced, so what runs next is
decided fresh from that poll's own findings, not blocked by comparing
against its own now-resolved priority.

Dropping a lower-priority `requestCarChargerRecheck()`/
`requestCarChargerV2hRecheck()` request never loses anything worth having:
since "Sync with `LoadControlTask.cpp`" above now fires on *every*
successful fetch regardless of which reason armed it, whichever poll
actually runs next — even one firing later than requested, for a
higher-priority reason that pre-empted it — still calls
`requestLoadControlPollNow()` the moment it lands with fresh data.

**`CarChargerTask.cpp`'s energy accumulation**: tracks actual elapsed time
between polls (`everPolled`/`lastPollMs`, `millis()` subtraction) rather
than assuming a fixed `POLL_INTERVAL_MS` — needed once the 20s recheck could
make consecutive polls much closer together than 5 minutes; a fixed-interval
assumption would have over-credited energy by ~15x on a recheck that caught
the charger mid-charge.

---

## 4. Ownership tracking — `LoadControlState.h`

The raw ON/OFF a device reports (`manualOn` for HW, `chargerOn` for the
car) is **not evidence a human did anything** — `LoadControlTask.cpp`'s own
toggles flip that exact same bit. `LoadControlState.h` tracks a separate
`AutoStarted` flag per load — "did WE (this control loop) start this" —
persisted to LittleFS (`/load_control_state.txt`) so an unscheduled reboot
(WiFi-drop watchdog reset, brownout) doesn't lose track of a still-running
load and leave it running unsupervised. Only written on an actual
start/stop transition, not every poll (LittleFS write wear).

A second flag, `carV2hRecoveryPending`, is
LittleFS-persisted the same way — written **before** each V2H toggle it
drives, not after — so a reboot landing mid-recovery still reflects what
the code was about to do rather than losing track of V2H having been
switched off. See §5a for the full recovery sequence this flag drives.

One further transient window is a RAM-only `pfodDelay` timer in
`LoadControlTask.cpp` (not persisted — losing it to a rare reboot is
low-stakes, unlike the `AutoStarted`/`carV2hRecoveryPending` flags
themselves): `carNoAcCooldownTimer` —
15min pause after a no-240VAC stop before retrying, so a car that's
unplugged (or finished charging) doesn't get immediately re-toggled every
poll. Queried two ways, both non-mutating: `carCheckStart()`'s own gating
check reads `.remaining()` directly (it needs the actual millisecond
value, for the "Xmin remaining" log line), while the display flag
published every cycle (`carInNoAcCooldown`, §6) uses the plain boolean
`.isRunning()` — `pfodDelay`'s own purpose-built "is this window still
open" check. Neither ever uses `justFinished()`, which is a one-shot
edge: it reads true exactly once, on whichever single poll happens to
land on the delay actually ending, then false on every poll after that
until the timer is restarted. Once it has fired, the delay is
definitively no longer active — but calling it again can never tell you
that a second time, so it's structurally unusable as a repeatable "is it
still open" query.

There is deliberately **no separate startup-grace timer** here:
`carCheckStart()`/`carCheckStop()` run in the
same `checkLoadControlPoll()` invocation, and `carCheckStart()` sets
`ctx.done=true` on the cycle it starts the car, so `carCheckStop()` never
even reaches its 240VAC check that cycle. By the time it next can (30s
later, §5), §3's 20s device recheck has already supplied the real state —
so nothing extra is needed to protect against reading a stale value too
soon.

---

## 5. The control decision — `LoadControlTask.cpp`

`checkLoadControlPoll()` runs the SOC-hysteresis decision logic every
`POLL_INTERVAL_MS` (**5.5 min**, or `EARLY_RECHECK_MS` = 30s right after a
car start, so the 240VAC check gets re-evaluated promptly against real
data — see §3's table for how this 30s decision-rerun timer differs from
§3's 20s state-refresh timer). Four phases, run in this exact order every
cycle — the order matters, since a stop decision in one phase can defer a
decision in a later one:

`POLL_INTERVAL_MS` here is deliberately longer than `CarChargerTask.cpp`'s/
`HotWaterTask.cpp`'s own 5min `POLL_INTERVAL_MS` — it barely matters in the common case, since §3's
"Sync with `LoadControlTask.cpp`" re-arms this timer for ~0ms after every
successful car fetch, so it only actually fires as a **fallback** (charger
unreachable, or before the first-ever sync). Were it the *same* 5min as
`CarChargerTask.cpp`'s own cadence, that fallback could race a fetch due at
almost the same moment and run against data about to be superseded moments
later — exactly the staleness the sync mechanism exists to close. Being
strictly longer guarantees that by the time this fallback ever does fire,
`CarChargerTask.cpp` has already had a full cycle to complete and publish,
so even the fallback path sees its freshest available data.

1. **`hwCheckStop()`** — stop HW if we started it and SOC has dropped below
   HW's own turn-off threshold; clear the `AutoStarted` flag (without
   stopping) if HW went off externally while we still thought we owned it.
2. **`carCheckStart()`** — start the car if SOC is above the car/EV's own
   turn-on threshold and nothing (cooldown, V2H-active) blocks it.
3. **`hwCheckStart()`** — start HW if SOC allows it — **skipped entirely**
   if HW was just stopped this same cycle (re-evaluate next poll instead of
   flip-flopping in one cycle).
4. **`carCheckStop()`** — stop the car if SOC has dropped, or if 240VAC
   disappeared (car unplugged/finished) — **deferred** if HW was stopped
   this same cycle, so at most one load changes state per poll.  

**SOC** = State of Charge — battery %

**Per-load turn-on/turn-off thresholds — `BatteryChargeSettings.h`**: HW
and the car/EV each have their own independently-settable SOC% turn-on
threshold (`hwSocOnPercent()`/`evSocOnPercent()`), rather than a single
shared pair both loads
apply alike. Each load's turn-off threshold is never
independently stored — it's always exactly 5% below that load's own
turn-on threshold (`hwSocOffPercent()`/`evSocOffPercent()`), so there is
only one persisted number per load. Both default to 35% whenever the saved
`/battery_charge_settings.txt` is missing, unreadable, or either value
parses outside the valid 10-100% range — a partially-valid file is treated
the same as a missing one and BOTH thresholds reset to 35%, rewriting the
file fresh, rather than patching one field. Same mutex-guarded/lazy-loaded
LittleFS pattern as `LoadControlState.h`, and for the same reason: read
from `LoadControlTask.cpp`'s background task, written from whatever task
services a pfodWeb HTTP request.

**A turn-on threshold of 99% or 100% means "disabled"**:
`LoadControlTask.cpp`'s `startConditionsMet(socPct, onPercent)` and
`keepConditionsMet(socPct, offPercent, onPercent)` both return `false`
unconditionally whenever `onPercent >= 99`, regardless of `socPct`/
`offPercent` — the load never starts, and is stopped on the very next poll
if it was already running (`keepConditionsMet()` takes `onPercent` as a
third parameter for this, not just `offPercent`, so a load left running
from before the threshold was raised to 99/100 doesn't keep coasting on
the ordinary "SOC hasn't dropped below offPercent yet" check). This makes
literal what the sub-menu's own prompt text already promises ("100%
settings => always off") — otherwise only true in practice if `socPct`
happened to read exactly `>=100.0` — and extends the same guarantee to
99%.

Settable live from the pfodApp main menu's **"HW/EV - Battery Charge%"**
sub-menu (`SubMenu_HW_EV___Battery_Char.h/.cpp`, generated; hooked up in
`SubMenu_HW_EV___Battery_CharApp.cpp` the same weak-override way
`pfodMainMenuApp.cpp` hooks the main menu) — two sliders, 'HW On at' and
'EV Charging at', each 10-100%. Moving a slider calls
`setHwSocOnPercent()`/`setEvSocOnPercent()` immediately, which updates the
in-RAM cache on the spot (so `LoadControlTask.cpp` sees it on its very next
poll and the sub-menu itself reflects the move instantly) but does **not**
write to LittleFS on the spot — a drag sends many intermediate slider
messages in quick succession, each a distinct value, and a real flash write
on every one of them would be exactly the write-wear
`LoadControlState.h`'s own comment already flags as worth avoiding.
**Echoing the clamped value back, not the raw one**: the generated
`handleCmd()` stores whatever raw value a client sends straight into
`pwm_HW_On_at_var`/`pwm_EV_Charging_at_var` (unclamped) before calling the
hook, then echoes that same variable back via `sendMenuUpdate()`
afterward — a client sending a raw `0` (below this slider's own
configured 10 minimum) would otherwise get that same `0` echoed straight
back, even though `setHwSocOnPercent()` had already correctly clamped its
own persisted copy to 10, a real mismatch between what's displayed and
what's actually stored. `onHwOnAtChanged()`/`onEvChargingAtChanged()`
(`SubMenu_HW_EV___Battery_CharApp.cpp`) also write the clamped value
(`hwSocOnPercent()`/`evSocOnPercent()`, read back right after persisting)
into that same variable before returning, so the echo — and hence what
the client's own slider visually snaps to — always matches what's
actually persisted.
Instead each change (re)starts a 1s debounce timer
(`BatteryChargeSettings.cpp`'s `saveTimer`); `checkBatteryChargeSettingsSave()`,
called every main-thread `loop()` (`BatterySolarMonitor.ino`, alongside
`checkMains()`), commits to LittleFS once the settings have been still for
that long — one write per settling, not one per tick. The sub-menu's own button on the main menu
shows the live values, "**Set at HW *n*% EV *n*%**" — recomputed fresh
right before every send, same "no separate push needed, just never stale
next time it's sent" convention `pfodMainMenuApp.cpp`'s own
`[Set Time/Timezone]` label already uses for the same reason (its
underlying value can also change from elsewhere without this menu being
told directly). **Shows "OFF" instead of the percent**
for whichever of HW/EV has its turn-on threshold at 99% or 100% —
`updateBatteryChargeLabel()` (`pfodMainMenuApp.cpp`) checks this
independently per load, matching the "disabled" threshold above (e.g.
"Set at HW OFF EV 35%" if only HW is disabled), rather than showing a
literal "99%"/"100%" that would read as just a very high number, not
"never turns on".

Every actual toggle goes through
`toggleCarChargerIfEnabled()`/`toggleHotWaterIfEnabled()`, never the raw
`toggleCarCharger()`/`toggleHotWater()` directly — these enforce the single
`ENABLE_LOAD_CONTROL` safety switch (defined in `BatterySolarMonitor.ino`,
**must** be `false` if any other controller could also be driving the same
hardware) and, on success, both:
- **Optimistically update** `CarChargerInstances`/`HotWaterInstances` with
  the intended post-toggle state (both the primary switch field and the
  secondary indicator — `ac240vActive`/`powered` — so the display doesn't
  show a stale "not charging"/"not heating" state, and so a stop doesn't
  leave a stale `ac240vActive=true` that would otherwise trip the FAULT
  branch below), without waiting for a re-fetch (an immediate re-fetch was
  tried first and rejected — it just as often caught the device mid-
  transition and published the wrong pre-toggle value with false
  confidence).
- **Trigger the 20s recheck** (§3) so the real state supersedes that
  assumption promptly.

### 5a. V2H trip recovery

The car charger's V2H (export) mode can **trip**: `v2h_state` stays `"on"`
while `ac240v_status` drops to `"inactive"` — the car's own V2H safety
circuit has taken its AC output offline. This is distinct from the car
V2H being manually turned off (`ac240vActive=false` with `v2hActive=false` too,
which `carCheckStop()`'s ordinary no-AC-stop branch already handles). A
trip is detected inside `carCheckStart()` (§5, phase 2), ahead of the
normal start logic, and runs its own retry loop:

1. **Trip detected** (`v2hActive && !ac240vActive`) → `setCarV2hRecoveryPending(true)`
   is written to LittleFS **before** the toggle is sent, then V2H itself is
   toggled OFF (`toggleV2hIfEnabled(false)`, POSTs `/v2hPower`). Nothing
   else happens this cycle — no early recheck is requested here, since
   turning V2H off is almost instant (same as the car charger's own
   on/off toggle), so even the next normal 5-minute cycle is comfortably
   longer than needed to see it take effect.
2. **Next cycle where `carV2hRecoveryPending()` is true and `v2hActive` now
   reads false** (confirms the OFF toggle actually took): first checks
   whether the charger's own schedule can be POSITIVELY CONFIRMED to want
   V2H on right now (below); only if so does it go on to check
   `v2hRelevantLoadKw()` — the whole-home metered load, **minus HW's own
   assumed 3.5kW draw when HW is powered** (`HW_ELEMENT_LOAD_KW`,
   `LoadStatusFormat.h`/`.cpp`) — since hot water is on a physically
   separate circuit from the car charger/V2H and shouldn't count against
   its capacity.
   - **`v2hScheduleConfirmsOn()` returns false** (below — covers "can't
     confirm", not just "confirmed off"): `setCarV2hRecoveryPending(false)`
     — standing down, no toggle sent — rather than attempting recovery.
   - **Load below `V2H_RETRY_MAX_LOAD_KW` (3.0kW)**: `setCarV2hRecoveryPending(false)`
     written to LittleFS first, then V2H is toggled back ON
     (`toggleV2hIfEnabled(true)`), optimistically assuming both `v2hActive`
     and `ac240vActive` come back on (same optimistic-update convention as
     §5's main toggle path) — reverted back to `true` if the toggle itself
     fails to even send, so LittleFS never claims recovery succeeded when
     it didn't send at all. On success, `requestCarChargerV2hRecheck()`
     (`CarChargerTask.h`) arms a **30s** recheck — longer than the ordinary
     20s post-toggle recheck (§3), since turning V2H back **on** takes
     roughly 2 × the charger relay's own ~8s settling time plus a few
     more seconds before AC actually starts (~16s+ in total) — unlike
     turning it off, which is almost instant (same as the car charger's
     own on/off toggle) — so the real post-recovery state gets confirmed
     rather than trusting the optimistic guess indefinitely.
   - **Load still ≥ `V2H_RETRY_MAX_LOAD_KW`**: stays pending, no toggle
     sent, nothing to undo — retries the same check again next cycle,
     indefinitely (there is no give-up).

**Catching an immediate re-trip promptly**: `requestCarChargerV2hRecheck()`
arms `CarChargerTask.cpp`'s 30s device recheck (§3's priority table). The
moment that recheck poll actually lands with fresh data, §3's general
"Sync with `LoadControlTask.cpp`" mechanism calls
`requestLoadControlPollNow()` — forcing a **full** `checkLoadControlPoll()`
cycle (all 4 phases) to run within about a second, so the re-trip check
re-evaluates against the CONFIRMED real result as soon as it's available,
rather than waiting on a fixed timer alone.

This explicit push replaces relying SOLELY on timing: a successful
recovery-on toggle still sets `CarCtx.v2hRecoveryAttempted = true`, which
still makes `checkLoadControlPoll()` re-arm its own poll timer to
`V2H_RECOVERY_RECHECK_MS` (**35s**, deliberately longer than the 30s device
recheck) — but that reschedule now only matters as a **fallback**, in case
the push never arrives (e.g. the charger is unreachable right when its
recheck is due, so `requestLoadControlPollNow()` is never called). In the
common case the explicit push fires first and this 35s timer's own eventual
firing is a no-op re-confirmation. Either way, a re-trip sends a fresh OFF
toggle (step 1 again) instead of sitting mis-reported as "recovered".

**Deferring to the charger's own V2H schedule, via a confirm-only gate**:
`EV_Charger_V2H_switch/switchSettings.cpp`'s own `handleAuto()` switches
V2H fully on/off at its own configured times (`v2h_time_on`/`v2h_time_off`,
when `v2h_auto_active`) — entirely independent of this device. Without
awareness of that schedule, a recovery left pending from an earlier real
trip could see the schedule's own Auto Off (a deliberate, scheduled
shutdown — V2H genuinely off) and
"recover" V2H straight back on again, fighting the schedule. This is why
`LoadControlTask.cpp`'s `v2hScheduleConfirmsOn(car)` is deliberately
fail-safe: recovery is attempted **only** when
the schedule can be positively confirmed to want V2H on, not merely
whenever it *isn't* confirmed off. Fetched alongside the rest of
`/api/state` (`CarChargerFetch.h`'s
`v2hAutoActive`/`v2hOnTimeMinutes`/`v2hOffTimeMinutes`), confirmation
requires ALL of:
- `v2hAutoActive` is true — a schedule that isn't even enabled confirms
  nothing;
- the raw on-time-to-off-time gap (wrapping past midnight if needed)
  exceeds **2 × the edge slack (4 minutes)** — equal on/off times, or
  times within a few minutes of each other, confirm nothing. This isn't
  just the degenerate equal-times case (which also matches `handleAuto()`'s
  own guard, never firing a transition on equal times either): shrinking a
  near-zero-width window inward by the slack from BOTH ends would flip the
  shifted on-time past the shifted off-time, which the wraparound math
  below would misread as a window covering nearly the whole 24 hours —
  exactly backwards from the tiny/no window the user actually configured.
  So this gap check must run *before* the slack is even applied, not rely
  on the window math to sort itself out;
- the clock has NTP-synced (`!missedSNTPupdate()`) — "now" is meaningless
  without a real clock;
- "now" falls inside the schedule's on-window, **`[onTime+2, offTime-2)`**
  (wrapping past midnight if `onTime > offTime`) — **2 minutes of slack at
  each edge** so this check doesn't race the charger's own per-minute
  `handleAuto()` tick (an off time of 7:02 or later withdraws confirmation;
  7:00/7:01 still confirm on. An on time of 14:00 doesn't confirm on until
  14:02).

Any single one of these failing means "can't confirm — don't recover":
step 2 above clears `carV2hRecoveryPending` instead of attempting
recovery, standing down and leaving V2H entirely to the schedule (which
turns it back on itself at its own on-time — this file does nothing
further). Because this check re-reads THIS cycle's freshly-fetched
`car.v2hOnTimeMinutes`/`v2hOffTimeMinutes` every poll (never a value
cached from whenever the recovery started), it also self-corrects
immediately if either the off-time boundary is crossed, or the schedule's
times are edited, while a recovery is still pending.

**Display**: the pending flag is published every cycle as
`CarChargerInstances.h`'s `carInV2hRecovery`, so `formatCarState()`
(§6) keeps showing "V2H tripped off" throughout the whole recovery
attempt even though `v2hActive` itself reads false in the meantime — a
raw read would otherwise be indistinguishable from V2H simply being off.
The load figure is shown only while genuinely tripped off (`v2hLoadKw`,
the same HW-adjusted figure the retry threshold itself checks against),
never while V2H is active and exporting normally.

### 5b. Blocking a start near the V2H window

§5a's trip recovery is *reactive* — it only engages after a trip has
already happened. The same real incident behind §3's sync fix showed a
*proactive* gap too: `carCheckStart()`'s phase 3 (Start) only ever looked
at the cached `car.v2hActive`, with no independent awareness of the
charger's own V2H schedule. Even with §3's sync fix, a start decision can
still land in the window between the schedule's on-time and
`CarChargerTask.cpp`'s next actual fetch confirming it (that incident's own
follow-up fetch didn't land until a minute after `carCheckStart()` had
already started the car) — starting a NEW charge session there puts a
real physical relay in direct conflict with the charger's own V2H safety
circuit about to (or having just) taken over the same AC output.

`nearV2hWindow(car)` closes this directly, without depending on fetch
freshness at all: it checks the charger's raw `v2hOnTimeMinutes`/
`v2hOffTimeMinutes` themselves, widened by `CAR_START_V2H_MARGIN_MIN`
(**2 minutes at EACH edge**, to allow for clock mismatch between this
ESP32 and the charger's own clock) — blocking from 2min *before* on-time
through 2min *after* off-time. `carCheckStart()`'s phase 3 checks this
first, and only when a start would otherwise actually happen (so a
low-SOC cycle that wouldn't have started anyway doesn't log a misleading
"blocked by V2H window" reason); if blocked, `ctx.carState =
CAR_STATE_V2H_WINDOW` and `carCheckStop()` reports "skipped (near V2H
window)". Only gates a **new** start (`!car.chargerOn`) — a charge session
already running into this window is `carCheckStop()`'s concern, unaffected
by this guard.

This is deliberately the mirror image of `v2hScheduleConfirmsOn()` (§5a):
that one *narrows* its confirm-on window inward by slack, because a false
negative there is cheap (just retry next cycle). Here a false negative
(failing to block) lets a real conflict happen, so the margin widens
*outward* instead — despite the coincidentally identical 2-minute value,
`CAR_START_V2H_MARGIN_MIN` is its own named constant, free to diverge from
`V2H_SCHEDULE_SLACK_MIN` if either is ever retuned. The fail-safe
*direction* also deliberately differs: with no synced clock
(`missedSNTPupdate()`), `nearV2hWindow()` returns `false` (don't block)
rather than `true` — an un-synced clock right after a reboot must not
permanently prevent charging whenever SOC is otherwise high enough, which
would be a worse regression than the narrow timing race this guards
against.

---

## 6. Status display — `LoadStatusFormat.h/.cpp`

`formatCarState()`/`formatHwState()` build the exact text
`Dwg_StatusApp.cpp`'s Status screen shows (and that `LoadControlTask.cpp`
logs every cycle, via the same two functions, so the device log always
matches what's on screen). Both take the `AutoStarted` flag as a parameter
(published into `CarChargerInstances`/`HotWaterInstances` by
`checkLoadControlPoll()` under the same locks as everything else) plus the
raw device state, and apply a 3-branch model — **the state tables are kept
as comments directly in `LoadStatusFormat.h`**, above the function
declarations; refer to those for the exact row-by-row text, not this doc,
so there's only one place to keep in sync. Summary of the branching logic:

1. **`autoStarted == true`** → label **Auto**, based purely on
   `powered`/`ac240vActive` (heating/charging or not) — the raw
   `manualOn`/`chargerOn` reading is ignored entirely here, since a
   transient off-reading while `autoStarted` is still true self-corrects
   within one poll cycle (it's not evidence of manual intervention).
2. **`autoStarted == false` and the raw switch reads ON** → label
   **Manual** — the one genuine signal that something other than this
   control loop engaged the load.
3. **`autoStarted == false` and the raw switch reads OFF** → still **Auto**,
   based on `powered`/`ac240vActive`. For hot water this can legitimately
   be "Auto off, heating" — the unit has its own independent solar
   auto-diverter path. For the car charger, the equivalent combination
   (`chargerOn=false` with `ac240vActive=true`) has no such second path, so
   it's flagged as a **FAULT** (`Car: <r>FAULT</r> 240VAC active while OFF`)
   rather than folded into the normal ladder.

`v2hActive` is checked first, unconditionally, ahead of all of the above —
a hardware safety block orthogonal to ownership.

---

## 7. Live forecast + shading lookup

`SolarForecastFetch.cpp` fetches Open-Meteo's hourly forecast directly from
the ESP32 (no PC involved) into `SolarForecast.h`'s hourly array — and
the daily temp max/min + rainfall forecast into `WeatherForecast.h`'s
instances in the SAME request/response (see §1a). `SunPosition.cpp`
computes elevation/azimuth per hour — the on-device equivalent of
`solar_shading_model.py`'s own `solar_position()`, same Spencer-equations
method.

**Fetch/timezone handling** — requests
`timezone=UTC&past_days=1&forecast_days=3` (4 raw UTC days), not
`timezone=<IANA name>&forecast_days=2`, because Open-Meteo's archive API
applies a single fixed, non-DST-aware UTC offset to an entire
IANA-timezone request — the same defect described in §1a and in
[`data_sources.md`](data_sources.md)'s "Timezone handling" section, which
means a simple assumption of index 0-23=today, 24-47=tomorrow (trusting
Open-Meteo's own IANA-timezone day alignment) can't be trusted either.
Instead, each returned `hourly.time[]` entry is parsed
(`parseIsoUtcToEpoch()`, a small UTC-civil-date-to-epoch routine,
deliberately NOT `mktime()`/`tzset()`, which assume LOCAL time) and
converted to local wall-clock time via `localtime_r()`; `dayOffsetFromToday()`
then places each hour into `today`'s or `tomorrow`'s own local hour slot
(or discards it, if it falls outside those two local days — the
`past_days`/`forecast_days` buffer). The `daily=` block needs the same
fix in miniature: `daily.time[]`'s index 0 is UTC yesterday, not local
today, so the target index is found by converting local noon
today/tomorrow to its own UTC calendar date and string-matching against
`daily.time[]`, rather than assuming a fixed position.

For each hour:

```
shadeFactor = shadeFactorForSunPosition(azimuthDeg, elevationDeg)   // ShadeFactorTable.h
kw = siteSystemKw() * gtiWm2 / 1000.0 * shadeFactor                 // SiteSettings.h, §1a
```

`ShadeFactorTable.cpp` loads `/shade_grid.csv` from LittleFS once at boot
(`loadShadeFactorTable()`, called from `setup()`) into a fixed-size array
(currently sized for 320 entries; 216 are typically populated by a
refit). `shadeFactorForSunPosition()` floors `(az, el)` to the same
10°/5° bins `solar_shading_model.py` uses and does a linear lookup;
**falls back to `0.85`** — the same default the PC-side model itself uses
(`apply_shading()`: `shade_factor.fillna(0.85)`) — for any bin never seen
in training, or if the table failed to load at all.

This lookup table, alongside `data/morning_floor.csv` (`MorningFloorTable.cpp`'s
own equivalent (month, hour) → clear-sky floor kW lookup, loaded at boot
the same way, same `0.85`-style "never seen in training" fallback
philosophy) and `site_settings.csv` (§1/§1a), is what this pipeline
(`BatterySolarMonitorUpdate/`) hands to the ESP32 — all three the same
way: `quarterly_update.py` writes `data/shade_grid.csv`
and `data/morning_floor.csv` (regenerated every run) to
`../shade_grid.csv`/`../morning_floor.csv` (one level above
`BatterySolarMonitorUpdate/`), alongside `../site_settings.csv` (which it
never writes at all — a genuine, entirely human-maintained file, edited
by hand only when site config actually changes). Nothing in this
pipeline writes directly into `BatterySolarMonitor/data/` — that folder
is only ever updated by a deliberate manual step, the same convention
`site_settings.csv` uses, never a background script silently
overwriting it on every run — so all three files are copied from
`PRIVATE_DIR` into `BatterySolarMonitor/data/` **by hand**, then synced
into the Arduino-IDE project and uploaded to the device's LittleFS
filesystem along with the next compile+flash (this project's standing
convention). `quarterly_update.py`'s own log output ends with an explicit
"MANUAL STEP REQUIRED" line naming all three paths, as a reminder not to
skip this.

---

## 8. Average consumption forecast + battery SOC projection

Two layers: `AvgConsumptionTask.cpp`'s `checkAvgConsumptionPoll()` is the
actual Enlighten fetch, so — like every other internet-facing job — it
runs on `InternetPollTask.cpp`'s shared background task (§2), in the tick
order right after `checkEnlightenHistoryPoll()`. `AvgConsumptionRefresh.cpp`
and `BatteryProjection.cpp` are pure local computation over data already
fetched elsewhere, with no internet connection of their own to
serialise against, so both instead run on the **main thread** via
`loop()` (`checkAvgConsumptionRefresh()`/`checkBatteryProjection()`,
called right after `checkEnlightenHistory()`) — the same
fetch-on-the-shared-task/translate-on-the-main-thread split
`checkSolarForecastRefresh()`/`checkGatewayRefresh()`/`checkEnlightenHistory()`
already use for their own data.

**`AvgConsumptionTask.cpp`/`AvgConsumptionInstances.h`
(`avgConsumptionProfile`)**: a running average of true home load (kW) per
15-min slot, built from the last 5 complete days' Enlighten history
(`EnlightenHistoryFetch.h`'s `fetchEnlightenConsumptionForDate()`) —
`AvgConsumptionProfile::addDay()` folds each successful day in via an
incremental mean, skipping any slot a given day doesn't have real data
for (rather than dragging that slot toward zero), so a bin only reflects
the days that actually had a reading there. Unlike every other job on
that shared background task, `checkAvgConsumptionPoll()` does **nothing**
on its own regular tick — it only runs when `requestAvgConsumptionFetch()`
has armed it, and then fetches all 5 days in one batch before going idle
again until the next request.

**`AvgConsumptionRefresh.cpp`** owns triggering that batch and turning
its result into chart data: once NTP has synced and the device has been
up briefly (25s, giving `EnlightenHistoryRefresh.cpp`'s own first "today"
fetch a head start on the same shared candidate-IP resolution), it
requests the one-shot 5-day batch above, then repaints `consumptionData`
(today, future slots only — never overwriting an already-recorded actual
reading) and `tomorrowAvgConsumptionData` (the whole day, since tomorrow
has no actual readings to protect yet) every time `successCount`
advances, until the batch finishes. `avgConsumptionForecastReady()`
(`lastPaintedSuccessCount > 0`) is the one flag other code should check
— **not** `avgConsumptionProfile.hasData()` directly, which can already
be true mid-batch, before anything has actually been painted onto the
chart yet. A total-batch failure (0 of 5 days succeed — confirmed to
happen for real when this feature's own readiness gate fires before
`EnlightenLoginFetch.h`'s boot-time login has finished building a session
cookie, so all 5 date fetches hit an empty `enlightenSession()`) is
retried every 5 minutes indefinitely; a **partial** success (≥1 of 5) is
deliberately left alone and never retried, since that's already a usable
average and retrying would only discard already-good data for no
benefit.

**`BatteryProjection.cpp`** projects battery SOC forward from "now" to
10pm (`PlotData::END_MINUTES`) in 15-min steps, writing the result into
`batteryChargeData` as the predicted (black) line on the Battery Chart —
a net-energy-balance approach: `net_kw = solar_kw − consume_kw` charges
the battery when positive (capped at the BMS's max charge rate, tapered
to half above 98% SOC, with a 93% charge efficiency loss applied) and
discharges it when
negative (no efficiency loss modelled), converted to a %-of-capacity step
each 15 minutes and clipped to `[BATT_BACKUP_SOC, 100]`. Runs every 5
minutes once its two preconditions are both met —
`avgConsumptionForecastReady()` above and `solarForecastReady()`
(§7) — and retries on the very next `loop()` tick (not a full 5-minute
wait) whenever a call bails out early (no current SOC reading yet,
outside the charted 5am-10pm window, or either forecast not ready), so
the *first* real projection happens promptly once both inputs actually
become available rather than up to 5 minutes late. Projecting from
`consumptionData`'s future slots before they hold a real average (still
Dwg_ConsumptionPlot1App.cpp's boot-time dummy-fill demo curve) would
produce a plausible-looking but entirely fabricated projection, which is
exactly what the `avgConsumptionForecastReady()`/`solarForecastReady()`
gates exist to prevent.

**DTG (discharge-to-grid) override**: while
`DischargeToGridState.h`'s `dtgPolledEnabled()` is true (§15) — the
ACTUAL polled Enphase state, not the button's own latch, so this also
covers the "disable failed, Enphase still shows it on" case §15 itself
flags — the normal net-energy-balance update above is skipped for every
15-min slot inside the DTG schedule window (`dtgScheduleStartMinutes()`/
`dtgScheduleEndMinutes()`, each snapped to this line's own grid via
`PlotData::nearestSlotMinutes()`) and replaced with a straight-line drop
from whatever % was projected (or actually measured, if the window
already started before "now") AT the window's start, down to the
schedule's OWN configured end SOC% (`dtgScheduleEndSoc()` — Enphase's
"limit" field, read from the same `schedules` response as the window
itself; the same % the schedule's own edit page on
enlighten.enphaseenergy.com shows/lets you set, not a hardcoded value)
EXACTLY at the window's end. Normal projection resumes for any slot
after the window, continuing from that %. Both the override's SOC target
and its final clamp use the SAME `[BATT_BACKUP_SOC, 100]` safety net the
normal path applies — `limit` is boundary data from Enphase, not a
trusted constant. Skipped entirely (normal projection throughout, no
override) if the window or its end SOC% hasn't been polled yet, the
window is degenerate after snapping to the grid, or the starting slot's
own % reads back as PlotData.h's own "no data" sentinel (0.0/-1.0) —
never interpolates from a fabricated starting % or guesses an end %.

**CFG (charge-from-grid) override**: the mirror-image of the DTG override just above, in every
respect except direction — while `ChargeFromGridState.h`'s
`cfgPolledEnabled()` is true (§16), a straight-line RISE from whatever %
was projected at the cfg window's start (`cfgScheduleStartMinutes()`/
`cfgScheduleEndMinutes()`) UP TO the cfg schedule's own configured target
SOC% (`cfgScheduleTargetSoc()` — Enphase's "limit" field for the cfg
schedule, charged UP to rather than discharged DOWN to) EXACTLY at the
window's end, same `[BATT_BACKUP_SOC, 100]` clamp, same "never guess"
skip conditions. Both overrides share the same per-slot loop; DTG is
checked first, so it wins if the two windows ever overlapped, though in
practice they don't (cfg mid-day, dtg evening).

**`requestBatteryProjectionRecompute()`** (`BatteryProjection.h`) — called
from `DischargeToGridTask.cpp`'s own background task right after every
`setDtgPolledEnabledAssumed()` call (§15's three optimistic-update sites:
boot/button disable, status-poll resend, and the auto-discharge decision)
so the DTG override above reflects an optimistic change immediately
instead of sitting on whatever it last computed for up to another
~1min (`PROJECTION_INTERVAL_MS`) — otherwise the SOC plot would lag
behind the button text even after that text itself started updating
immediately, since this file's own throttle is independent of
`DischargeToGridState.h`'s. Mutex-protected (genuinely cross-thread);
`checkBatteryProjection()` picks the request up and forces
`projectionTimer` due on its very next call, ahead of the normal
`isRunning()`/`justFinished()` pair.

---

## 9. Car charge history — `CarChargeHistory.h/.cpp`

`CarChargerInstances.h`'s `carChargingKwhToday` is a pure ESP32-side RAM
estimate — the charger exposes no real energy reading at all, so this
device accumulates it itself from elapsed on-time at an assumed charge
rate (§3). Unlike the Solar/Consumption/Battery charts, which
re-backfill from Enphase's own cloud history on every boot and so look
continuous across a reboot regardless of uptime, nothing backs this
estimate at all — an unplanned reboot (WiFi-drop watchdog reset,
brownout) would otherwise silently zero it with no way to tell
afterwards that real charging had already happened earlier that day.


Two small LittleFS files, both written from `CarChargerTask.cpp`'s own
poll (§3) and requiring `lockCarChargerState()` already held by the
caller, same as every other access to that state:

- **`/car_charge_today.csv`** — today's own `(date, running kWh so far)`,
  overwritten every time `carChargingKwhToday` actually changes (at most
  once per ~5min poll). Read back at the *first* poll with a synced clock
  each boot (`loadOrResumeCarChargeToday()`) to resume today's total
  after an unplanned reboot instead of losing it — if the stored date is
  no longer today (the device was off across a midnight), that stored
  total is banked as a finished day first, since it never got a graceful
  rollover before the device went down.
- **`/car_charge_history.csv`** — up to 5 completed days' final totals,
  oldest first. Appended either gracefully, at a genuine local-midnight
  rollover detected while running (`CarChargerTask.cpp`'s own date-changed
  check calls `rolloverCarChargeDay()`), or retroactively at the next
  boot via the missed-rollover path above.

`printCarChargeHistory()` formats the up-to-5-day history plus today's
own running total as one multi-line block (oldest first, today last, a
day the device was off for entirely simply absent rather than shown as a
misleading 0) — used by `DebugLogBuffer.cpp` at the top of the Log Tail
screen (a separate debug-output feature from this file, not covered
elsewhere in this doc).

---

## 10. Buzzer alarm — `BuzzerControl.h/.cpp`

Runs entirely on the **main thread**, called every `loop()` as `checkMains()`
(right after `processNTP()` in `BatterySolarMonitor.ino`) — unlike §2-§5,
this is not one of `InternetPollTask.cpp`'s background jobs. Two independent
alarm reasons share one physical buzzer pin (`buzzerPin`, `D7`), so each
reason is tracked as its own latched boolean and the pin itself is only ever
driven once per loop, from the OR of the two.

**Mains-loss alarm** — `mainsAlarmActive`, latched:
- **Set**: on the falling edge of `mainsDetectorSw` (`DebouncedSwitch` on
  `D11`) — i.e. the instant
  loss of voltage on D11 is debounced-confirmed.
- **Cleared**: mains voltage returns to D11 (`mainsDetectorSw.isDown()`
  reads false again), **or** the pushbutton (`sw`, `D2`, grounded via `D6`
  as its dedicated GND) is pressed — either silences it, per spec.
- Always enabled — no time-of-day gate, no cancel-latch that could leave it
  permanently disarmed.

**Low-SOC alarm** — `socAlarmActive = inWindow && !socAlarmCancelled &&
(isSocLow() || socFetchStuck())`:
- **Window**: local time between 14:00 and 20:00 (`SOC_ALARM_START_MIN`/
  `SOC_ALARM_END_MIN`), via `LocalTimeMinutes.h`'s
  `localTimeMinutesSinceMidnight()` — fails safe to "not in window" (no
  alarm) if the clock has never NTP-synced (`missedSNTPupdate()`), the same
  fail-safe convention `LoadControlTask.cpp` (§5b) uses for its own
  time-of-day checks.
- **SOC reading**: `latestBatterySocPercent` (`GatewayInstances.h`), read
  under `lockGatewayReading()`/`unlockGatewayReading()` (`GatewayPollTask.h`)
  since it's written by the background `InternetPollTask` and read here on
  the main thread — same access pattern `Dwg_StatusApp.cpp`/
  `GatewayRefresh.cpp` already use. The `-1` "last fetch failed/not yet
  known" sentinel fails safe to "not low" (`isSocLow()` alone never alarms
  on it).
- **Sustained-failure alarm** (`socFetchStuck()`): true once
  `GatewayPollTask.h`'s `consecutiveBatteryFetchFailures()` reaches
  `SOC_ALARM_FAIL_RETRY_COUNT` (20) — i.e. the battery-SOC fetch has failed
  20 times in a row, each `FETCH_RETRY_DELAY_MS` (15s, `GatewayPollTask.cpp`)
  apart, so 5 minutes of not being able to confirm the SOC at all. This
  guards against a run of genuine gateway connection failures immediately
  followed by one HTTP-200 response whose device list has no usable `soc`
  field on it (gateway not fully back up yet). `GatewayFetch.cpp`'s
  `fetchBatteryStatus()` validates each device's `soc`/`real_power_mw`
  fields rather than trusting them blindly: a missing/null field fails the
  whole fetch (returns `false`) rather than defaulting it to `0.0` via
  ArduinoJson's `| 0.0` fallback — a silent default there would be accepted
  as a genuine "SOC=0.0%" reading, plotting a false 0% point on the
  Battery Charge chart and immediately sounding `isSocLow()`'s alarm off a
  single bogus data point. The same applies if a field is present but not
  actually a float (`!d["soc"].is<float>()`/`!d["real_power_mw"].is<float>()`),
  and a device record reporting `soc == 0.0 && real_power_mw == 0.0`
  together is also rejected, as the signature of an uninitialized/zeroed
  record rather than a genuine reading — a real battery can't plausibly be
  at 0% while drawing/supplying exactly 0W simultaneously, and Enphase's
  own configured backup reserve keeps a real battery from legitimately
  reaching 0% at all under normal grid-connected operation; a device
  that's merely LOW with nonzero power is not rejected. Every one of these
  failure modes feeds `consecutiveBatteryFetchFailures()`, so
  `socFetchStuck()` still gives a real, eventually-fires alarm path for a
  *genuinely* prolonged outage, without a single stray or bogus reading
  ever sounding the alarm on its own.
- **Cancel latch** (`socAlarmCancelled`): set true by a pushbutton press
  **only while the low-SOC alarm is actually sounding at that moment**
  (`socAlarmActive` computed BEFORE the button is applied, same
  already-active precondition the mains-loss alarm's own cancel uses);
  reset to false the moment local time leaves the window. This is what
  gives "once turned off, does not turn on again between 2pm and 8pm" (a
  press latches silence for the rest of *that* window) while still
  re-arming "at startup and between 2pm and 8pm if not already cancelled
  during that interval" — starts `false` at boot (static initialisation)
  and re-arms itself automatically every time the window is re-entered on a
  later day. SOC naturally recovering above 15% without a button press is
  **not** treated as "turned off" for latching purposes — the alarm just
  stops (condition no longer met) and can resume later the same window if
  SOC drops again; only an explicit press while it's sounding arms the
  latch. This precondition matters because both alarms share the one
  pushbutton: latching `socAlarmCancelled` on any press while merely
  inside the window, regardless of whether the low-SOC alarm was actually
  sounding, would let a press meant only to silence a concurrent
  mains-loss alarm also silently arm this latch for the rest of the
  window even though SOC had never dropped low at all.

The pushbutton is level-checked (`sw.isDown()`), not edge-checked, every
loop — holding it down keeps both `mainsAlarmActive` false and
`socAlarmCancelled` true for as long as it's held, which is harmless
(idempotent).

---

## 11. Daily reboot — `DailyReboot.h/.cpp`

`checkDailyReboot()` (main thread, every `loop()`) reboots the device
once, shortly after local midnight each day, purely as routine
housekeeping — clearing any heap fragmentation or small leaks that may
have accumulated over a full day of WiFi/HTTPS/JSON churn, rather than
letting them run unbounded across weeks of continuous uptime. Several
other features documented above quietly depend on this happening once a
day: §1b's self-refreshed Enlighten session naturally gets re-checked
every reboot, comfortably inside the session cookie's ~14-day freshness
window, and §9's car-charge history treats "the device was off across a
midnight" as a distinct case from an in-progress local-midnight rollover.

**Detection**: compares each call's `localTimeMinutesSinceMidnight()`
against the *previous* call's — a real midnight rollover is exactly
"today's reading is smaller than last time's" (23:59 → 00:00 wraps ~1439
down to ~0). No-op until NTP has synced at least once
(`missedSNTPupdate()`), so an un-synced clock's startup value is never
mistaken for a real midnight, and the very first post-sync check only
*records* the current reading rather than comparing against it — a device
that happens to boot right after a real midnight must not mistake its own
first reading for a rollover and reboot-loop.

**The non-obvious part**: a plain "did minutes decrease" check would also
fire on the once-a-year DST autumn fallback (AEDT→AEST), when the clock
itself jumps backward 60 minutes around 2-3am — a genuine decrease in
minutes-since-midnight, but not a midnight rollover. The spring-forward
transition only ever jumps the clock *forward*, so it can't cause a false
trigger here, but the fallback needed a real fix: the rollover check
requires the drop to exceed `MIN_ROLLBACK_MINUTES` (90) before triggering
a reboot, since a real midnight wrap is always a much larger drop (up to
~1439) than a 60-minute DST fallback, telling the two apart regardless of
exactly when either check happens to run relative to the transition.
Once a reboot fires, it can never fire again immediately afterward either
— the fresh boot session starts back at "haven't seen a reading yet" with
no previous value to compare against.

---

## 12. Debug output + Log Tail — `TimestampStream.h`, `DebugLogBuffer.h/.cpp`

Every `debugPtr->print()`/`println()` call anywhere in this project — on
the main thread or any background task — writes through one shared
`TimestampStream` instance, which does two things to every byte before
deciding where it goes: prefixes each line with a local-time
`[HH:MM:SS] ` timestamp (suppressed entirely until NTP has synced at
least once, rather than printing a plausible-looking but wrong time that
would make an ordinary startup delay look like a multi-hour freeze), and
unconditionally forwards it to `DebugLogBuffer.h`'s
`debugLogBufferWrite()` — regardless of whether a real output stream
(`setDebugOut()`, typically `Serial`) is even set, so turning Serial off
never turns off the Log Tail screen (pfodWeb's live "Raw Data" mechanism)
too; the two destinations are fully independent.

**Why a whole line needs its own lock, not just each byte**: this stream
is shared by every background poll task at once, and a single logical
debug line is usually built from several separate `print()` calls (e.g.
`print("x="); print(x); println("y")`). Without serialising across the
*whole* line, two tasks' lines can interleave mid-line — confirmed in
practice as genuinely garbled log output (one task's text spliced into
the middle of another's). `debugLogLineMutex()` (`DebugLogBuffer.h`,
since `TimestampStream` is header-only/inline with nowhere of its own for
a shared definition to live) is taken on a line's first byte and given
back only on that line's own `'\n'`, so a second task starting its own
line simply blocks until the first finishes rather than interleaving into
it. The lock/line-position tracking (`atLineStart`/`haveLock`) is
`thread_local`, not a plain shared field — a real, previously-hit bug
otherwise let one task see *another* task's `haveLock` state and call
`xSemaphoreGive()` on a mutex it never took, crashing the device outright.
The lock wait itself is bounded (1s), not indefinite, so a task that dies
mid-line (or a bug that never writes the closing `'\n'`) can't
permanently wedge every other task's debug output — timing out just
means that one line proceeds without exclusive access instead of
freezing the whole stream forever.

**`DebugLogBuffer.h`** owns where each byte actually ends up:
`debugLogBufferWrite()` forwards straight through to pfodWeb's own
`getRawDataWriter()` relay once the web server exists to receive it, or
to a small capture buffer (`preWebServerBuffer`) before that point. That
pre-webserver capture matters because `BatterySolarMonitor.ino` calls
`loadEnphaseAuth()`/`loadSiteConfig()` — whose own "loaded X/N
keys"/unexpected-extra-key WARNING messages are exactly the kind of thing
worth seeing in Log Tail — well before `start_pfodWebServer()` has run
and allocated the relay's backing buffer; writes before that point would
otherwise vanish silently, reaching only the physical Serial console.
`notifyWebServerStarted()` (called once, immediately after
`start_pfodWebServer()` in `setup()`) just latches `debugLogBufferWrite()`
over to the now-valid relay for everything written from that point on —
it no longer flushes `preWebServerBuffer` anywhere itself.

**Opening the screen**: `debugLogBufferDumpAll()` (called from
`pfodMainMenuApp.cpp`'s `onLogTailPressed()` override) prints the
`{=Last 16K of Debug Log~C}` open-screen response, `CarChargeHistory.h`'s
`printCarChargeHistory()` output next (§9), THEN re-prints
`preWebServerBuffer`'s own captured startup text — every time the screen
is opened, not just once at boot (this device reboots once a day at midnight — §11 — so by the
time anyone actually checks Log Tail, often hours later, e.g. the
following morning, ordinary logging has almost certainly pushed a
one-time boot-time flush out of the relay's own 16K window through
perfectly normal eviction, long before any client's own cursor ever
reaches it; confirmed in practice this way). `preWebServerBuffer` is
never written to again once `notifyWebServerStarted()` latches, so
re-reading it (`resetForRead()`, which only rewinds the READ position —
the underlying bytes are untouched) always reproduces the same boot-time
text regardless of how long ago boot actually was. Only after that does
`debugLogBufferDumpAll()` reset *THIS requesting client's own* read
cursor into the shared relay — each polling client tracks its own
independent cursor/position, so opening or re-opening the screen on one
device can never affect what any other currently-connected viewer sees
or has already read.

---

## 13. Enlighten history backfill — `EnlightenHistoryFetch.h/.cpp`

The local IQ Gateway only ever reports "right now" (§1) — nothing about
already-elapsed hours earlier today. `EnlightenHistoryFetch.cpp` closes
that gap by fetching today's already-elapsed solar/consumption/
battery-SOC history (15-min slots from midnight to now) from Enphase's
Enlighten cloud instead, in one call (`production.meter`, `consumption`,
and `acb.acb_soc_ts` all share the same `start_date` + `i*900s` slot
scheme). `EnlightenHistoryTask.cpp`'s `checkEnlightenHistoryPoll()` is the
actual fetch, so it runs on `InternetPollTask.cpp`'s shared background
task (§2); `EnlightenHistoryRefresh.cpp`'s `checkEnlightenHistory()` owns
triggering it and merging the result into the live chart data, on the
main thread — the same fetch-on-the-shared-task/translate-on-the-main-thread
split §8 uses for `AvgConsumption`/`BatteryProjection`.

**Why this keeps re-running every 5 minutes, not just once near boot**:
the local gateway's own points (`GatewayPollTask.cpp`) are *instantaneous*
readings taken every 5 minutes — spikier than Enphase's true 15-min
*average* for the same slot. Each successful refresh's `mergeHistoryInto()`
overwrites those instantaneous points with Enphase's smoother true
average for the elapsed slots it covers, and — since this fetch goes to
Enphase's cloud rather than the local gateway — also transparently
backfills any stretch where the local gateway connection itself was down.
`REFRESH_INTERVAL_MS` is 5 minutes, deliberately shorter than Enphase's own
15-minute history slot resolution, to bound the worst-case delay in
picking up a newly-available slot: a refresh landing just before the
cloud side advances to a new slot would otherwise mean waiting almost a
full 15 more minutes to pick it up; 5min caps that worst-case pickup
delay at ~5min. Most individual refreshes
still just re-confirm a slot that hasn't advanced yet (a harmless,
idempotent no-op) — that's expected and fine, since the point is
bounding the worst case, not the typical case.

**A real bug this design deliberately avoids**: `mergeHistoryInto()`
never copies a slot past "now"'s own currently-resolved point, regardless
of how much history the fetch actually returned. Without that limit, a
15-min-old fetch's own history array can still hold trailing zero/stale
values for slots that haven't happened yet from *that fetch's* point of
view, and copying those over `solarData`/`consumptionData`/
`batteryChargeData` would overwrite an already-correct current reading
with a stale one, mis-colored as "actual" — confirmed for real (a
captured chart showed the Battery Charge line's actual/orange colour
extending to ~95% well past the true current ~60% reading, staying wrong
until `BatteryProjection.cpp`'s own next 5-minute cycle happened to
repaint those same slots back to predicted/black). `GatewayRefresh.cpp`
(writes only *at* "now") and this merge are the only two writers of
actual-coloured chart data, so this is the one place that guard has to
live.

**A real external routing problem it works around**:
`enlighten.enphaseenergy.com` is a CNAME to an AWS load balancer with
several backend IPs, DNS round-robined between them — and one of them was
confirmed, via a direct TCP connect test, to simply hang rather than
connect from this network (a real routing problem external to the ESP32
or its credentials, not a bug here). Since the ESP32's lwIP DNS resolver
only ever returns one address per lookup (confirmed on real hardware,
unlike a full OS resolver), `resolveEnlightenHosts()` re-resolves before
*every* retry, not just the first attempt — each fresh lookup has a
decent chance of landing on a different, working IP rather than trusting
a single cached answer that might be the bad one. `fetchEnlightenHistoryToday()`/
`fetchEnlightenConsumptionForDate()` connect via the IP directly (the real
hostname passed separately as the TLS SNI/Host header, the same trick
`SolarForecastFetch.cpp` uses), so this fetch itself never triggers a DNS
lookup — only `resolveEnlightenHosts()` does, and that must only ever be
called from the main thread, same `dns_clear_cache()` hazard §1b
explains in full for `resolveSolarForecastHost()`.

`AvgConsumptionTask.cpp` (§8) reuses this same file's
`fetchEnlightenConsumptionForDate()` for its own 5-day rolling average —
one fully-elapsed *past* day at a time, the whole day (no "stop at now"
cutoff, since it's already over), consumption only. A missing/null slot
in that response is left as `-1.0` (a real "no data" sentinel, not a
fabricated `0.0`), so `AvgConsumptionProfile::addDay()` can correctly
exclude it from that slot's average instead of blending in a fake zero
reading.

---

## 14. Car charger / hot water wire protocols

§3/§5/§5a/§5b describe the fetch/task/control architecture and what each
field is used *for*; this section is the API/protocol reference itself —
the actual endpoints, commands, and response shapes `CarChargerFetch.cpp`/
`HotWaterFetch.cpp` parse.

### Car charger — HTTP, JSON (`CarChargerFetch.h/.cpp`)

Three endpoints on the charger's own local LAN IP (`siteCarChargerIp()`,
§1a):

| Endpoint | Method | Purpose |
|---|---|---|
| `/api/state` | GET | Returns the JSON fields below |
| `/chargerPower` | POST (empty body) | Toggles the charger relay on/off |
| `/v2hPower` | POST (empty body) | Toggles V2H (export) mode on/off |

`/api/state` response fields:

| Field | Meaning |
|---|---|
| `charger_state` | `"on"`/`"off"` — the charger relay itself |
| `ac240v_status` | `"active"`/`"inactive"` — whether the charger's AC output is actually live (distinguishes "on but nothing plugged in/finished" from genuinely charging) |
| `v2h_state` | `"on"`/`"off"` — V2H (export) mode |
| `v2h_auto_active` | `"active"` when the charger's own scheduled Auto on/off is enabled |
| `v2h_time_on` / `v2h_time_off` | `"HH:mm"` — the charger's own configured V2H schedule boundaries (§5a's `v2hScheduleConfirmsOn()` reads these fresh every poll, never a cached copy) |

`charger_state`/`ac240v_status`/`v2h_state` are required — a response
missing even one is treated as unreachable, not "definitely off" (see
§3's "Sync with `LoadControlTask.cpp`" for why a stale/wrong reading here
would be worse than an honest "couldn't tell"). `v2h_auto_active`/
`v2h_time_on`/`v2h_time_off` are not: a missing `v2h_auto_active` fails
safe to "false" (matching `nearV2hWindow()`'s own fail-safe direction,
§5b), and a missing/malformed time string parses to `0` (midnight),
matching that field's own documented "0 == disabled/unset" convention.

### Hot water controller — raw TCP, pfod protocol (`HotWaterFetch.h/.cpp`)

Not HTTP — a plain TCP socket to `siteHotWaterIp():4989` (§1a), speaking
the same pfod message protocol (`{...}`, self-delimiting by the closing
`}`) the ESP32's own pfodWeb UI uses. Every exchange after connecting
needs the firmware version prefix first:

| Step | Send | Typical response | Meaning |
|---|---|---|---|
| Version handshake | `{.}` | ``{,<bg w>~`2500~V3|+A~c1}`` | Version is the text between the 2nd and 3rd `~` (e.g. `V3`) — prefixes every command below |
| Channel states | ``{<ver>:c2`0}`` | ``{+|C`13~9~3~~|h`1|...|uh`7|...}`` | ``|C`13~9`` = channel 13 (manual latch) is on; ``|uh`7`` = channel 7 (the element itself) is powered, ``|h`7`` = not powered |
| kWh today | ``{<ver>:c4`0}`` | ``{+|t`16~0~<+4><b>Hot Water 3.8kWhr~~}`` | Number between `Hot Water` and `kWhr` |
| Fully-heated flag | ``{<ver>:c5`0}`` | ``{+|t`17~2~<+3><b>Hot Water was Fully Heated~~}`` | Presence of `Fully Heated` in the response |
| Heating source | ``{<ver>:c8`0}`` | ``{+|t`19~12~<+4><b>Solar Hot Water/n2:01pm~~}`` | One of `Solar Hot Water`/`Off Peak Hot Water`/`Hot Water OFF`/`No WiFi Connection`, matched by substring |
| Toggle manual latch | ``{<ver>:A~c3`1`0`0}`` | (ack) | Flips channel 13's current manual state — see the safety note below |

The channel-state query (`c2`) is required — a failure fails the whole
fetch, since it's the field the control logic depends on (§5). The kWh
(`c4`), fully-heated (`c5`), and source (`c8`) queries are each
best-effort: a failure on any one of them just leaves that field at its
default (`0.0`/`false`/unknown) rather than failing the whole fetch.

**Toggle safety guard**: the toggle command flips whatever the manual
latch's *current* state is — there's no separate "set on"/"set off"
command — so every caller must read `manualOn` from a fresh `c2` fetch
before deciding whether to send it, exactly the same "check current state
before toggling" pattern §5's own `toggleCarChargerIfEnabled()`/
`toggleHotWaterIfEnabled()` use. Separately, the controller itself
auto-clears the manual latch after a fixed timeout on its own side (a
hardware/firmware behaviour of the hot water controller, not something
this ESP32 controls or can observe directly) — meaning `manualOn` can
read `false` even though this device's own `AutoStarted` flag (§4) still
thinks it started the load, purely because enough time passed since the
last toggle. §4/§5 already treat "flag says we started it, but the raw
switch now reads off" as "cleared, not stopped" for exactly this reason,
not as a bug to fix here.

---

## 15. Discharge-to-grid button — `DischargeToGridState.h`/`DischargeToGridFetch.h`/`DischargeToGridTask.h`

The main menu's "Auto Battery Charge/Discharge Enabled/Disabled" button
(`onAutoBatteryChargeDisPressed()`, `pfodMainMenuApp.cpp` — renamed from
`onBatteryCanDischargeTPressed()` when a pfodWeb Designer regeneration
split the old single button into this button plus a separate status
label, see "Button/label text" below) controls whether
the battery is **allowed** to discharge to the grid during Enphase's own
DTG (discharge-to-grid) schedule window — separate from
`checkDischargeToGridLogic()` (`DischargeToGridTask.cpp`, below), which
decides *when* to actually turn Enphase's own setting on while that's
allowed. Three new files, the same Fetch/Task split as §3, plus a fourth
for the persisted latch:

- **`DischargeToGridState.h/.cpp`** — the overall "Auto Battery
  Charge/Discharge" master switch's own latch (`autoDischargeEnabled()`),
  persisted to `/discharge_to_grid_state.txt` the same way
  `LoadControlState.h` (§4) persists its own flags, plus the latest polled
  Enphase status (`dtgPolledEnabled()`/`dtgScheduleStartMinutes()`/
  `dtgScheduleEndMinutes()`/`dtgScheduleEndSoc()` — RAM only, recomputed
  every poll, never persisted; the window is kept as raw
  minutes-since-midnight, not a display string, so `BatteryProjection.cpp`'s
  own DTG override below can align it to its 15-min plot grid directly —
  `DischargeToGridFetch.h`'s `formatMinutesOfDay()` converts it to
  "8:01pm" style for display). This latch ALSO gates §16's Hot Day
  decision, not just discharge-to-grid, and persists across every reboot,
  including this device's own daily midnight one (§11) — it is not
  force-reset at boot (see below).
- **`DischargeToGridFetch.h/.cpp`** — pure networking against the SAME
  Enphase Enlighten cloud `batteryConfig` API the
  `enlighten.enphaseenergy.com` browser page's own DTG toggle uses
  :
    - `fetchDischargeToGridStatus()` — `GET .../battery/sites/{site}/schedules`
      for the DTG window AND its own configured end-of-window SOC%
      (`dtg.details[0].startTime`/`endTime`, reformatted "8:01pm" style
      to match the web page; `dtg.details[0].limit` — the SAME % the
      schedule's own edit page on enlighten.enphaseenergy.com
      shows/lets you set), then `GET .../batterySettings/{site}` for the
      current `dtgControl.enabled` flag. Both `limit` AND `enabled` are
      checked for TYPE, not just presence — a wrong-type `limit` (fails
      the whole fetch if it's missing, not actually a JSON number, below
      `BATT_BACKUP_SOC` (10%, `BatteryConstants.h` — the setting can
      never validly go below the battery's own backup reserve), or above
      100%) or a wrong-type `enabled` (not actually a JSON boolean) would
      otherwise silently convert to `0.0`/`false` via ArduinoJson's own
      default-on-mismatch behaviour — for `enabled` specifically, that
      wrong `false` reads as "Enphase confirms discharge off", which
      both `DischargeToGridTask.cpp`'s resend check and
      `BatteryProjection.cpp`'s drop-to-`limit`% override would act on,
      not just a value nobody uses. Fails (returns `false`, leaves every
      out-param untouched) rather than guess, on any network/parse
      problem, an invalid `limit`/`enabled`, or if no DTG schedule is
      configured at all.
    - `setDischargeToGridEnabled()` — `GET .../schedules` (for a fresh
      `X-Csrf-Token`) → `POST .../schedules/isValid`
      (`{"scheduleType":"dtg"}`, returns a new token) →
      `PUT .../batterySettings` (`{"dtgControl":{"enabled":true|false}}`).
      Only returns `true` once the PUT itself comes back
      `{"message":"success"}` — the site then applies the change
      asynchronously (confirmed by hand against the live account:
      ~5-20s later), so a caller shouldn't assume it's already live the
      instant this returns.
  Authenticates with the same `enlightenSession()`/`enlightenSystemId()`
  (`EnphaseAuth.h`) §1b's self-refresh already keeps current, over a
  `Cookie: _enlighten_4_session=...` + `e-auth-token` header pair — the
  same pair `checkEnlightenTokenValid()` (§1b) already uses — and calls
  `notifyAuthTokenRejected()` (§1b) on a confirmed 401, same as every
  other Enlighten-cloud caller. Reuses `enlightenHostIP()` (§1b) instead
  of a third independent DNS resolve of the exact same hostname.

  **`POST isValid` returning 403 — root cause found and confirmed fixed.**
  `GET`s always returned 200, but `setDischargeToGridEnabled()`'s
  `POST .../schedules/isValid` consistently came back 403
  . Two header-matching guesses
  first (`Origin`/`Referer`, then `User-Agent`/`Username`/`Requestid` —
  all present in the real browser's HAR captures and in
  `test/enphase_dtg_toggle.py`, both confirmed working) made no
  difference — worth having added
  regardless, but not the
  actual cause. Found by inspecting a real login's cookie jar directly
  (`python -c "import enphase_dtg_toggle as t; ..."`, dumping
  `http.cookiejar` contents before/after a real GET): the GET to
  `.../schedules` ALSO sets a `BP-XSRF-Token` COOKIE, carrying the exact
  same value as the `X-Csrf-Token` response header/`X-XSRF-Token` request
  header — a double-submit CSRF check that compares the header against
  THIS cookie, not a server-side session store, which
  `test/enphase_dtg_toggle.py`'s own cookie-jar-based opener satisfies
  transparently (invisible in its own explicit `headers` dict, which is
  why reading that script's source alone missed it). Confirmed by
  replicating the ESP32's own exact approach in Python — manually-built
  `Cookie` header, no cookie jar at all — end to end against the live
  account: `GET -> 200`, `POST isValid -> 200`, `PUT -> 200
  {"message":"success"}`, and a follow-up `status` read (after the usual
  ~20-50s asynchronous apply delay) confirmed `dtgControl.enabled` had
  genuinely gone to `false`. Fix: `sendBatteryConfigRequest()` mirrors
  `xsrfToken` into the `Cookie` header (`; BP-XSRF-Token=<token>`)
  whenever it's non-empty — no separate `Set-Cookie` parsing needed, since
  the cookie and header always carry the identical value.
  `sendBatteryConfigRequest()` also now logs the response body (truncated
  to 300 chars) on any non-200, which is what made the generic Spring
  Security `{"status":403,"error":"Forbidden"}` body visible in the first
  place — enough to rule out the two header guesses, though the actual
  cause needed the cookie-jar inspection to find.
- **`DischargeToGridTask.h/.cpp`** — runs on the shared background task
  (§2), three jobs:
    - a ONE-SHOT "send Enphase's own setting off" request, due
      immediately on this task's very first tick (Enphase's own setting is
      always disabled on restart; the auto-discharge decision logic then
      takes over from there) and re-armed on demand by
      `requestDischargeToGridDisableNow()` (the button handler, when the
      user switches to disabled). NOT retried on its own — fired once,
      optimistically assumed to have worked.
      Immediately BEFORE firing the request, calls
      `setDtgPolledEnabledAssumed(false)` (`DischargeToGridState.h`) AND
      `requestBatteryProjectionRecompute()` (§8) so the button text and
      the SOC plot's own DTG override both reflect it right away, rather
      than sitting on whatever was last actually polled/projected for
      however long `setDischargeToGridEnabled()`'s own GET/POST/PUT chain
      takes (confirmed several seconds in practice) — let alone until the
      next ~5min status poll happens to run.
      `disablePending` is genuinely written from two tasks — this
      background task AND whatever task services the button press — so
      it's mutex-protected, the same two-caller situation §4's own
      `carV2hRecoveryPending` describes for the "Clear Recovery Flag"
      button. The lock is held only around the flag itself, never across
      `setDischargeToGridEnabled()`'s own multi-second network call —
      same "fetch unlocked, publish locked" split `checkEnlightenHistoryPoll()`
      (§13) uses, so a button press is never left waiting behind a slow
      HTTPS round trip.
    - a recurring 5min status poll (`fetchDischargeToGridStatus()`),
      publishing into `DischargeToGridState.h` via `setDtgPolledStatus()`.
      A FAILED poll retries after 30s instead of the full 5min (same role
      as §2's `FETCH_RETRY_DELAY_MS`) — otherwise a session that's still
      mid-refresh right after boot (§1b's own login/validate cycle starts
      at a 15s uptime floor) could leave the button showing stale/default
      placeholder text for up to 5 minutes. **This poll is also the
      one-shot disable's own retry mechanism**: `runStatusPoll()` reads
      the button's latch BEFORE `setDtgPolledStatus()` can force it (see
      below), and if Enphase still reports discharge on, resends
      `setDischargeToGridEnabled(false)` whenever the latch wants it off,
      the mandatory boot disable hasn't been confirmed off yet
      (`bootDisableUnconfirmed`, cleared permanently the first time a poll
      sees `enabled == false`), OR the auto-discharge decision below
      hasn't positively decided to turn discharge ON — again just assumed
      to work, with the same `setDtgPolledEnabledAssumed(false)` AND
      `requestBatteryProjectionRecompute()` (§8) calls fired BEFORE the
      resend itself, same reasoning as the one-shot disable above
      (overriding the `enabled=true` this same poll's own
      `setDtgPolledStatus()` call just published a moment earlier, so the
      display never settles on that stale reading even transiently), so
      a still-failing disable self-corrects every ~5min for as long as
      the mismatch persists, instead of a dedicated fast-retry loop.
      `bootDisableUnconfirmed` is deliberately a SEPARATE flag from the
      latch check: the latch (`DischargeToGridState.h`) is persisted and
      not force-reset at boot — if it happens to be in its ordinary
      "enabled" state (the common case) going into a reboot, checking
      only the latch would give the mandatory boot disable no resend
      coverage at all if its one fire-and-forget attempt failed to land,
      leaving the boot safety action with strictly weaker retry coverage
      than a user's own button press. Cleared ONLY by this
      poll actually observing `enabled == false` for real — never by the
      optimistic `setDtgPolledEnabledAssumed()` calls, which affect only
      the button/plot display, not this safety gate
      (`checkDischargeToGridLogic()` below deliberately keeps requiring
      real confirmation here).

      **The third resend condition — the auto-logic's own pending/negative
      decision — closes a separate, real gap found in practice** (no
      "auto-discharge logic ... -> enabling discharge" line was ever
      logged, yet the evening DTG window still showed the drop-to-25%
      projection): once `bootDisableUnconfirmed` clears and the latch
      allows discharge, nothing previously kept re-asserting "off" if
      `checkDischargeToGridLogic()` hadn't positively decided to turn
      discharge ON (either because it hadn't decided yet, or because it
      had decided OFF) — Enphase's own cloud-side schedule re-engaging on
      its own went completely uncorrected, since the old resend condition
      (`wantedOff || bootDisableUnconfirmed`) only ever checked the
      button's own latch, never the auto-logic's actual intent. Skipped
      once the schedule window has actually started today (a plain
      `nowMinutes >= dtgScheduleStartMinutes()` check, re-evaluated fresh
      each poll, same as `checkDischargeToGridLogic()`'s own identical
      check) — this poll shouldn't keep fighting Enphase's own setting
      once that function has stopped caring either, per its own "ignore
      changes during and after the discharge schedule starts" rule.
      `haveDtgDecision`/`lastDtgDecision` (previously declared only next
      to `checkDischargeToGridLogic()`) moved up alongside
      `bootDisableUnconfirmed` so `runStatusPoll()` can read them too —
      both still only ever touched from this same background-task tick,
      so still no lock needed.
    - the auto-discharge DECISION itself (`checkDischargeToGridLogic()`,
      ticked right after the status poll above) — see "The auto-discharge
      decision" further below for the full rule, cadence and threading.

  `setDtgPolledStatus()` never touches the `autoDischargeEnabled()` latch,
  in either direction. While the latch is disabled, discharge-to-grid and
  charge-from-grid status are still reported, but the latch is never
  automatically re-enabled. The recurring status poll still actively
  resends a disable while the latch is off and Enphase reports discharge
  on, correcting the actual Enphase setting back toward off, but it never
  flips the latch itself back to enabled as a side effect of doing so.

**Button/label text — split into TWO items by a pfodWeb
Designer regeneration** (previously one single button,
`button_Battery_can_dis_Cmd`/`var`, now gone entirely from the generated
files): a plain status LABEL (`label_Battery_will_di_Cmd`/`var`) plus the
actual clickable BUTTON (`button_Auto_Battery_Ch_Cmd`/`var`, hook
`onAutoBatteryChargeDisPressed()`). Both recomputed fresh before every
`sendMainMenu()`/`sendMainMenuUpdate()`, same pattern as
`updateTzFormatLabel()`/`updateBatteryChargeLabel()`.

`updateBatteryStatusLabel()` (`pfodMainMenuApp.cpp`) — `label_Battery_will_di_var`.
Lines 2-4 are not gated by the master switch latch (`autoDischargeEnabled()`)
at all — both status polls run every background-task tick independent of
it, so these lines keep reporting Enphase's own actual polled state
(lines 3/4, always) or the Hot Day forecast/schedule (line 2's own
"tomorrow" cases) the same way whether the latch is enabled or disabled;
only the DECISION logic itself (`checkDischargeToGridLogic()`/
`checkHotDayLogic()`) is gated by it. Line 1 is the one exception,
checking the latch first (see its own row below):

| Line | Text |
|---|---|
| 1 (discharge-to-grid) | "Battery will not discharge⏎(auto disabled)" if the master switch (`autoDischargeEnabled()`) is currently `false` (⏎ = a line break, so the "(reason)" always sits on its own line below the main text) — takes priority over `dtgPolledEnabled()` below so this never falls through to a possibly-stale "will discharge" reading from the brief window before the resulting disable request has actually been confirmed off; else "Battery **will** discharge to `<end%>`% between `<start>` and `<end>`" if Enphase's own `dtgControl.enabled` is currently `true` (`<end%>` is `dtgScheduleEndSoc()`, "?" if not polled yet, times via `appendDtgWindowDetail()`); else reports SPECIFICALLY which of `checkDischargeToGridLogic()`'s own gates (§15 below) is currently withholding discharge, tested in the SAME order that function itself tests them, so the label can never name a reason that isn't really the (first) one actually blocking it: tomorrow forecast hot (`HotDayState.h`'s `readHotDayForecast()`) — "Battery won't discharge tonight⏎(hot day forecast)"; else tomorrow forecast cold (`tomorrowColdDayForecast()`, reading `tomorrowWeatherForecast.tempMaxC` directly against `DischargeToGridTask.h`'s own `coldDayMaxTempC()`) — "Battery won't discharge tonight⏎(cold day forecast)"; else tomorrow's forecast excess too low (`tomorrowExcessTooLow()`, against `minTomorrowExcessKwh()`) — "Battery won't discharge tonight⏎(tomorrow's forecast excess <5kWh)"; else today's car charging too low (`carChargeTodayTooLow()`, against `minCarChargeTodayKwh()`) — "Battery won't discharge tonight⏎(car charge today <10kWh)"; else none of the gates are blocking it, so it WILL discharge — "Battery will discharge to `<end%>`% between `<start>` and `<end>`" (whole line red like the `dtgControl.enabled==true` case above, just not ALSO bold — this is the decision logic's own intent, not yet confirmed by an actual Enphase poll, but still a "will discharge" statement, so still red; no parenthetical reason, so no line break here). Every case in this line whose text says "will discharge" (both red cases above) colours the WHOLE line red via `<r>...</r>`, not just the word "will"; the cases that say "will not discharge"/"won't discharge" are not red. The three threshold numbers are read from `DischargeToGridTask.h`'s own `coldDayMaxTempC()`/`minTomorrowExcessKwh()`/`minCarChargeTodayKwh()` rather than duplicated as separate magic numbers, so the displayed figures can never drift out of sync with the real decision's own fixed constants. |
| 2 (charge-from-grid) | Also reports the Hot Day forecast, not just today's live on/off — `tomorrowHot`/`tomorrowReady` (`HotDayState.h`'s `readHotDayForecast()`, read ONCE and shared with line 1 above) drives both; the target% comes from `appendCfgTargetSocPercent()` (shared with line 4, so the two can never show different %s for the same `cfgScheduleTargetSoc()`). Every "will charge from grid..." case is a WHOLE LINE red (`<r>...</r>`, same shape as line 1's own red cases), "will" also bold for the live/confirmed case only; "will not charge from grid" is NOT red: if `cfgPolledEnabled()` (§16) is `true` — "Battery **will** charge from grid to `<target%>`% between `<start>` and `<end>`" (`ChargeFromGridState.h`'s `cfgScheduleStartMinutes()`/`cfgScheduleEndMinutes()`/`cfgScheduleTargetSoc()`), PLUS, if `tomorrowReady && tomorrowHot` too (today's own Hot Day carrying into tomorrow's), a SECOND line, also red but not bold: "Battery will charge from grid to `<target%>`% tomorrow between `<start>` and `<end>`"; else if `tomorrowReady && tomorrowHot` — "Battery will charge from grid to `<target%>`% tomorrow between `<start>` and `<end>`" (red, not bold — a plan, not yet live); else "Battery will not charge from grid" (plain, not red). "Today" and "tomorrow" read the SAME recurring cfg schedule window — there's no separate "tomorrow's schedule" to fetch. |
| 3 (Enphase discharge-to-grid raw status) | A plain, raw readout of Enphase's own actual polled `dtgControl.enabled`, UNCONDITIONALLY — unlike line 1 above, never takes the master switch or any gate into account. A single `<i>` opens here, left unclosed through to the end of line 4 (the `\n` between them doesn't reset the markup, so one opening tag covers both lines): "*Enphase Discharge to Grid down to* `<end%>`*% between* `<start>` *and* `<end>`" (`appendDtgWindowDetail()`) if `dtgPolledEnabled()`, else "*Enphase Discharge to Grid disabled*". |
| 4 (Enphase charge-from-grid raw status) | Same shape as line 3, for charge-from-grid, continuing the same italic span opened by line 3's `<i>` (left unclosed, since this is the last line of the whole label): "Enphase Charge from Grid up to `<target%>`% between `<start>` and `<end>`" (`appendCfgWindowDetailWithTargetSoc()`) if `cfgPolledEnabled()`, else "Enphase Charge from Grid disabled". `appendCfgWindowDetailWithTargetSoc()` is built from `appendCfgTargetSocPercent()` (the "`<target%>`%" part — the mirror-image of line 3's own `dtgScheduleEndSoc()`) + `appendCfgWindowDetail()` (the window part); the same `appendCfgTargetSocPercent()` line 2 above also uses, so lines 2 and 4 can never show a different % for the same underlying `cfgScheduleTargetSoc()`. |

`updateAutoBatteryButtonLabel()` (`pfodMainMenuApp.cpp`) —
`button_Auto_Battery_Ch_var`, the master switch toggle itself, from the
latch alone (replacing the OLD single button's own "Battery will/can
discharge to `<end%>`% between `<start>` and `<end>`" / "Disable Auto
Battery Discharge" text, which is now line 1 of the status label above
instead):

| Latch | Text |
|---|---|
| enabled | `<g><b>Auto Battery Charge/Discharge Enabled</g>` / "Click here to Disable" |
| disabled | `<r><b>Auto Battery Charge/Discharge Disabled</r>` / "Click here to Enable" |

**The auto-discharge decision — `checkDischargeToGridLogic()`
(`DischargeToGridTask.h`/`.cpp`, runs on `InternetPollTask.cpp`'s shared
background task, right after `checkDischargeToGridPoll()` — see the
threading note below for why it lives there and not in
`pfodMainMenuApp.cpp`).** While the button's own latch
(`autoDischargeEnabled()`) is enabled, decides whether Enphase's own
setting should be on, re-checking every 5 minutes until the DTG schedule's
own start time arrives each day:

- **Rule**: computes
  tomorrow's forecast excess (`tomorrowSolarData.totalKwh()` minus
  `tomorrowAvgConsumptionData.totalKwh()`, the average over however many
  of the last 5 days have actually been fetched so far — the SAME
  instances §8's `BatteryProjection.cpp` and the Tomorrow chart already
  use, with the same 1-5-day partial-data tolerance) and today's
  car-charging energy so far (`CarChargerInstances.h`'s
  `carChargingKwhToday`, read under `lockCarChargerState()`). Enables
  Enphase's setting only if the excess is at least `MIN_TOMORROW_EXCESS_KWH`
  (5kWh — replacing the previous plain ">0" threshold) AND today's car
  charging is at least `MIN_CAR_CHARGE_TODAY_KWH` (10kWh); otherwise it
  disables it. Both are fixed constants in `DischargeToGridTask.cpp`, not
  adjustable settings.
- **Only acts on a CHANGE**: re-computes every 5 minutes but only
  requests a change when the computed decision actually differs from the
  last one requested — never just because 5 minutes passed and the same
  answer came out again.
- **Display updates immediately, assumed to have succeeded**: BEFORE
  calling `setDischargeToGridEnabled()` (not after — the GET/POST/PUT
  chain it runs can take several seconds, and there's no reason to
  leave the display stale for that whole round trip when the outcome is
  being assumed anyway), calls `setDtgPolledEnabledAssumed()`
  (`DischargeToGridState.h`) so the button's "will"/"can" text reflects
  the decision right away, instead of sitting stale for up to ~5 minutes
  until the next real status poll happens to run. If
  the assumption is wrong (the call silently failed), the next real
  status poll's own `setDtgPolledStatus()` unconditionally overwrites it
  with Enphase's actual state regardless — same "assume success, let the
  next real poll correct it either way" convention the boot/button
  disable action already relies on. Doesn't touch the schedule window
  times (unaffected by this decision) or the latch (already true, by
  construction, for this function to be running at all). Immediately
  followed by `requestBatteryProjectionRecompute()` (§8) — without it,
  `BatteryProjection.cpp`'s own DTG override reads the same
  `dtgPolledEnabled()` but only every ~1min on its own independent
  throttle, so the SOC plot would still lag behind the button text by up
  to that long even after this fix.
- **Stops once the window starts**: `nowMinutes >= dtgScheduleStartMinutes()`
  is re-evaluated fresh every call, not a sticky "frozen" flag — once
  true, no further changes are requested for the rest of that day,
  self-correcting regardless of any button toggling in the meantime and
  resetting naturally at the next local midnight (in the ordinary case,
  the next daily reboot too).
- **Disabled latch**: doesn't run at all — `DischargeToGridTask.cpp`'s own
  boot/button-disable path owns turning Enphase off in that case. Resets
  its own state when the latch goes false, so a later re-enable starts
  completely fresh and begins requesting changes again immediately.
- **Boot safety gate**: also holds off requesting "on" until the mandatory
  boot disable has actually been confirmed off (`bootDisableUnconfirmed`,
  read directly — same file, same thread, no cross-thread read needed).
- **Hot Day gate —  also
  skips the normal decision entirely whenever `HotDayState.h`'s
  `readHotDayForecast()` reports tomorrow's forecast as hot
  (`tempMaxC >= tomorrowsMaxTempTriggerC()`), re-tested fresh on EVERY
  tick, independent of §16's own once-a-day 2:02pm gate: "if forecast max
  temp tomorrow is >= setting then disable discharge to grid (if it is
  enabled)". Resets `haveDtgDecision` when this gate is hit, same shape as
  the disabled-latch branch, so the existing resend-on-mismatch poll
  (`runStatusPoll()`'s `wantedOff` check) keeps discharge forced off for
  as long as a hot day stays forecast, with no separate one-shot "disable
  now" call needed here. A "forecast not ready yet" read retries at 0
  rather than falling through — never guesses hot/not-hot from unready
  data. See §16 for the full Hot Day feature.
- **Cold Day gate**: also
  skips the normal decision entirely whenever tomorrow's forecast max
  temp (`tomorrowWeatherForecast.tempMaxC`, read directly under
  `lockSolarForecast()`/`unlockSolarForecast()` — the same fields/lock
  the Hot Day gate's own `readHotDayForecast()` reads, not reused as a
  function since this is a separate, `DischargeToGridTask`-only
  threshold) is at or below `COLD_DAY_MAX_TEMP_C` (18°C, a fixed constant, not
  an adjustable setting like Hot Day's own `tomorrowsMaxTempTriggerC()`).
  Same "reset `haveDtgDecision`, let the existing resend-on-mismatch poll
  keep discharge forced off" shape as the Hot Day gate above. A "forecast
  not ready yet" read retries at 0, same as every other not-ready gate in
  this function.

**Threading — why the decision lives in `DischargeToGridTask.cpp`, not
`pfodMainMenuApp.cpp`**  an earlier attempt had
this backwards, with the decision in `pfodMainMenuApp.cpp` and a
request/apply handshake into the background task; that handshake is gone
now. `setDischargeToGridEnabled()` itself must stay confined to
`InternetPollTask.cpp`'s one shared background task (§2's own reason for
existing: avoiding overlapping internet-facing connections), so the
decision that calls it directly — no handoff — lives there too:
`checkDischargeToGridLogic()` is called right after
`checkDischargeToGridPoll()` on every background-task tick, reading that
same poll's own `dtgScheduleStartMinutes()`.

Its two INPUT numbers are read directly too,
not through a throttled-refresh cache: `tomorrowSolarData.totalKwh()`/
`tomorrowAvgConsumptionData.totalKwh()` (`PlotDataInstances.h`, gated on
`avgConsumptionForecastReady()`/`solarForecastReady()`) — the SAME two
totals `Dwg_StatusApp.cpp`'s own status line and `BatteryProjection.cpp`
already read straight off. An earlier version of this design added a
throttled-refresh-plus-mutex cache in `pfodMainMenuApp.cpp` specifically
to avoid a cross-thread read of these main-thread-written totals; that
cache (and its own separate mutex) was removed,
replaced with a single lock that covers both the read side here AND the
write side — `PlotDataInstances.h`'s `lockTomorrowForecastData()`/
`unlockTomorrowForecastData()`. `checkDischargeToGridLogic()` holds it
across BOTH readiness checks AND both `totalKwh()` calls as one atomic
snapshot, so it can never see a readiness flag already true while the
matching `SolarForecastRefresh.cpp`/`AvgConsumptionRefresh.cpp` paint (main
thread) is still in progress. Those two files hold the same lock around
painting their own instance AND setting its own readiness flag together.
The lock is RECURSIVE (`xSemaphoreCreateRecursiveMutex`) because
`solarForecastReady()`/`avgConsumptionForecastReady()` also take it
internally, so they stay safe to call standalone from their existing
main-thread-only callers (`Dwg_StatusApp.cpp`, `BatteryProjection.cpp`,
unchanged) — a plain mutex would deadlock the moment
`checkDischargeToGridLogic()` holds the lock across its own call into
those same functions. `batteryChargeData`/`solarData`/`consumptionData`
(the other three `PlotDataInstances.h` instances) are only ever touched
from the main thread, so deliberately not covered by this lock.

**Battery SOC forecast**: §8's own `BatteryProjection.cpp` reads
`dtgPolledEnabled()`/`dtgScheduleStartMinutes()`/`dtgScheduleEndMinutes()`/
`dtgScheduleEndSoc()` too, overriding its normal projection to a
straight-line drop to the schedule's own configured end SOC% (its
"limit" field, not a hardcoded value) by the schedule's end whenever
Enphase is actually discharging — see that section's own "DTG override"
paragraph.

---

## 16. Hot Day — `HotDayState.h`/`ChargeFromGridState.h`/`ChargeFromGridFetch.h`/`HotDayTask.h`

On a forecast-hot tomorrow,
pre-charges the battery from the grid (Enphase's own `chargeFromGrid`
toggle, a DIFFERENT Enphase feature from §15's discharge-to-grid) and
switches the HW/Car start% over to separate, higher Hot Day thresholds —
instead of letting that evening's ordinary discharge-to-grid schedule run
at all. Continuing §15's Fetch/State/Task split:

- **`BatteryChargeSettings.h/.cpp`** — three more
  persisted fields alongside the existing `hw_on`/`ev_on`:
  `max_temp_trigger` (`tomorrowsMaxTempTriggerC()`/
  `setTomorrowsMaxTempTriggerC()`, 20-40°C, matching the "Tomorrows Max"
  slider's own range), `hot_day_hw_on`/`hot_day_car_on`
  (`hotDayHwOnPercent()`/`hotDayCarOnPercent()` + their own `OffPercent()`
  siblings (-5%, same convention as the originals), 10-100%, matching the
  two Hot Day sliders' own range). Validated as its OWN all-or-nothing
  group, SEPARATE from `hw_on`/`ev_on` — grouping all five into one check
  would silently reset every existing device's already-valid `hw_on`/
  `ev_on` to their own defaults too, purely because these three newly-added
  fields are (quite normally) absent from an older saved file; keeping
  them separate means an upgrade from the old 2-field file just adds the
  three new ones at their own defaults, untouched otherwise. Default
  values, used on a genuinely first boot or whenever this group is
  missing/unreadable/out of range (`HOT_DAY_DEFAULT_MAX_TEMP_TRIGGER_C`/
  `HOT_DAY_DEFAULT_HW_ON_PERCENT`/`HOT_DAY_DEFAULT_CAR_ON_PERCENT`):
  30°C / 95% / 90%.
- **`HotDayState.h/.cpp`** — `hotDayActive()`/`setHotDayActive()`: the
  APPLIED-state latch, persisted to `/hot_day_state.txt`. Unlike
  `DischargeToGridState.h`'s `autoDischargeEnabled()`, this is NEVER
  force-reset at boot, it survives exactly as left, including this
  device's own daily midnight reboot (§11). Also exposes
  `effectiveHwSocOnPercent()`/`effectiveHwSocOffPercent()`/
  `effectiveEvSocOnPercent()`/`effectiveEvSocOffPercent()` — the Hot Day
  settings if `hotDayActive()`, else the ordinary defaults;
  `LoadControlTask.cpp`'s `hwCheckStop()`/`hwCheckStart()`/
  `carCheckStart()` (§5) call these INSTEAD of `BatteryChargeSettings.h`'s
  plain accessors, so load-control gating transparently follows whichever
  settings are currently active without needing to know about Hot Day
  itself. And `readHotDayForecast()` — compares `WeatherForecastInstances.h`'s
  `tomorrowWeatherForecast.tempMaxC` (under `SolarForecastTask.h`'s
  `lockSolarForecast()`) against `tomorrowsMaxTempTriggerC()`; returns
  `false` (never guesses) if the forecast hasn't arrived yet. Read fresh
  on EVERY tick by TWO different callers and this section's own
  `checkHotDayLogic()` below (but only ACTED on once a day, at the 2:02pm
  gate).
- **`ChargeFromGridState.h/.cpp`** — `cfgPolledEnabled()`/
  `cfgScheduleStartMinutes()`/`cfgScheduleEndMinutes()`/
  `cfgScheduleTargetSoc()`/`setCfgPolledStatus()`/
  `setCfgPolledEnabledAssumed()`: the `chargeFromGrid` counterpart of
  `DischargeToGridState.h`'s polled-status fields (`cfgScheduleTargetSoc()`
  mirrors `dtgScheduleEndSoc()` -- Enphase's own "limit" field for the cfg
  schedule, but charged UP to rather than discharged DOWN to) — RAM only,
  no persistence, and no latch of its own to force: whether
  `chargeFromGrid` SHOULD be on is entirely `hotDayActive()`, not a
  separate button. Mutex-protected, since `BatteryProjection.cpp`'s own
  CFG override (§8) reads these fields from the main thread -- a genuine
  cross-task read against `HotDayTask.cpp`'s own background-task tick
  writes, same shape as `DischargeToGridState.h`'s own mutex-guarded
  fields.
- **`ChargeFromGridFetch.h/.cpp`** — pure networking against the SAME
  Enphase Enlighten cloud `batteryConfig` API. Same
  `sendBatteryConfigRequest()` shape as `DischargeToGridFetch.cpp`,
  including that file's own `BP-XSRF-Token` double-submit cookie mirror
  (the fix for DTG's own `POST isValid` 403, above) -- carried over on the
  inference that both schedule types share the identical CSRF filter
  chain (same API family, same `X-Csrf-Header`/`X-Csrf-Param` response
  shape confirmed for both in the HAR). The cfg feature differs from dtg
  in two confirmed ways:
    - `fetchChargeFromGridStatus()` — `GET .../battery/sites/{site}/schedules`
      for the "cfg" schedule's own `startTime`/`endTime` (`HotDayTask.cpp`
      adds 2 minutes to `endTime` for its own daily decision trigger —
      read dynamically, not hardcoded, so it tracks whatever window is
      actually configured on Enphase's own schedule-edit page, same
      reasoning as §15's `dtgScheduleStartMinutes()`) AND its own `limit`
      (the target SOC% charged UP to by the window's end,
      `BatteryProjection.cpp`'s own CFG override above reads this —
      checked for type/range (0-100%) same as §15's own `limit` check,
      just without that one's `BATT_BACKUP_SOC` floor, which is specific
      to the discharge direction), then `GET .../batterySettings/{site}`
      for the current TOP-LEVEL `chargeFromGrid` boolean — NOT nested
      under a `cfgControl` object the way discharge-to-grid nests under
      `dtgControl`.
    - `setChargeFromGridEnabled()` — `GET .../batterySettings` (for a
      fresh `X-Csrf-Token` AND the current `cfgControl.forceScheduleOpted`
      flag — checked for type, boundary data, never assumed) →
      `POST .../schedules/isValid`
      (`{"scheduleType":"cfg","forceScheduleOpted":<that value>}` — cfg's
      isValid call needs this extra flag echoed back; dtg's doesn't) →
      `PUT .../batterySettings` (`{"chargeFromGrid":true|false}`, a
      top-level field, not nested).
- **`HotDayTask.h/.cpp`** — runs on the shared background task (§2), right
  after §15's two functions. Two jobs, same shape as
  `DischargeToGridTask.h`'s own pair:
    - a recurring ~5min status poll (`fetchChargeFromGridStatus()`),
      publishing into `ChargeFromGridState.h` via `setCfgPolledStatus()`,
      then resending `setChargeFromGridEnabled()` whenever the polled
      `chargeFromGrid` flag doesn't match `hotDayActive()` — covers BOTH
      the ordinary ongoing target AND `requestChargeFromGridDisableNow()`'s
      own one-shot "force off now" request (armed by
      `pfodMainMenuApp.cpp`'s `onAutoBatteryChargeDisPressed()` the
      instant the master switch is turned off), since that request only
      ever means "force off", which is also what `hotDayActive()==false`
      already wants — no separate resend condition needed. A FAILED poll
      retries after 30s instead of the full 5min, same role as §2's
      `FETCH_RETRY_DELAY_MS`.
    - the Hot Day DECISION itself (`checkHotDayLogic()`) — runs only while
      the master switch (`autoDischargeEnabled()`) is enabled (disabled:
      resets its own daily-decided flag and stops, same shape as §15's
      own disabled branch — the immediate "disable both and revert to
      default" transition is `pfodMainMenuApp.cpp`'s own job, not this
      function's). Before the daily trigger time (2 minutes past
      `cfgScheduleEndMinutes()`), does nothing but keep
      re-testing on a 5min cadence (same "early-exit retries at 0" startup
      fix as §15's own logic). Once that time is reached, decides AT MOST
      ONCE PER DAY (a RAM-only `tomorrowHotDayDecided` flag — named for
      what it tracks: the decision made at TODAY's trigger is always
      about TOMORROW's forecast, never today's). Unlike §15's own
      `haveDtgDecision`/`lastDtgDecision` (which don't need a daily reset
      for correctness, just a clean slate, so a reboot-only reset is
      fine), this flag is the ONLY thing stopping a second decision the
      same day, so it's cleared by a plain time comparison instead — every
      day, before its own trigger time is reached — rather than relying on
      this device's own daily midnight reboot (§11) to reset it, which
      could otherwise leave Hot Day permanently stuck after the first
      decision if a reboot was ever missed. Reads `readHotDayForecast()`
      and calls `setHotDayActive(isHot)` — persists, no-op if already
      matching — then optimistically assumes `setChargeFromGridEnabled(isHot)`
      will succeed (`setCfgPolledEnabledAssumed()`, same "assume success,
      let the next real poll correct it either way" convention as §15
      throughout), fire-and-forget, with the status poll above as its own
      retry mechanism.

**On restart (including the daily midnight reboot)**: if `hotDayActive()`
was persisted `true`, the Hot Day HW/Car% (`effectiveHwSocOnPercent()`
etc.) are already in effect from the moment `LoadControlTask.cpp` first
reads them — no boot-time re-application needed, `HotDayState.h`'s own
persistence already covers it. `chargeFromGrid` itself is re-asserted by
the ordinary resend-on-mismatch poll above (due immediately on this
task's very first tick, same `statusPollTimer.start(0)` idiom as every
other poll timer in this project) if Enphase's own side didn't survive
the reboot confirmed — no separate dedicated one-shot boot action needed,
unlike §15's mandatory "always disable on boot" rule, since there's no
single fixed target here independent of `hotDayActive()` to re-assert.

**Master switch disable transition — `pfodMainMenuApp.cpp`'s
`onAutoBatteryChargeDisPressed()`**: switching the master switch off now
performs the WHOLE "stop check and disable both discharge to grid and
charge from grid and switch HW/car back to the default setting"
transition in one shot:
`requestDischargeToGridDisableNow()` (§15, unchanged) +
`requestChargeFromGridDisableNow()` (this section's own one-shot) +
`setHotDayActive(false)` (immediate — HW/Car% revert to default right
away, not left on the Hot Day settings until the next 2:02pm).

**HW/EV - Battery Charge% sub-menu button text**
(`updateBatteryChargeLabel()`, `pfodMainMenuApp.cpp`) — shows the Hot Day
HW/Car% settings and trigger temperature alongside the existing defaults,
then a trailing italic call-to-action line: `<b>`"Set at HW `<hw%>` EV
`<ev%>`" / "Hot Day >`<triggerC>`C HW `<hotHw%>` EV `<hotEv%>`"`</b>` /
`<i>`"Click here to change settings" — each percentage shown as "OFF"
instead of a literal "99%"/"100%", same rule as the original two-field
version of this label.
