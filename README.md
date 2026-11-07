# PATH Train Clock

An ESP32 + 60-LED WS2812B ring that shows the next PATH trains from
**Grove Street → 33rd Street (ToNY)** as a clock face.

## How it works

- **60 LEDs = 60-minute clock face.** LED 0 at top = 12 o'clock.
- **Clock hands:** purple hour, green minute, white second (NTP synced).
- **Train hands:** point to the minute-of-hour each train arrives.
  - **Live** trains (from the PATH API) = darker orange.
  - **Scheduled** trains (from the timetable) = lighter amber.
  - **ATW** (via Hoboken) trains blink orange ↔ blue.
  - The **next train** pulses (breathes), speeding up as it approaches,
    then turns solid red under 4 minutes.
- **Service alerts:** if PATH posts a delay alert for the JSQ-33 line, the
  whole ring breathes red underneath everything else.

## Display reference

| What you see | Meaning |
|--------------|---------|
| Train LED pulsing, speeding up | Next train approaching |
| Train LED solid red | Next train < 4 min away |
| Steady amber LEDs | Scheduled trains |
| Darker orange LED | Live (real-time) train |
| Orange ↔ blue blinking LED | ATW train (via Hoboken) |
| Whole ring breathing red | Service alert on the JSQ-33 line |
| Purple / green / white LEDs | Hour / minute / second hands |

## Data sources

| Source | What | Frequency |
|--------|------|-----------|
| `ridepath.json` | Live next-2 trains | every 30s |
| `alerts.json` | Service delay alerts | every 2 min |
| `schedule.json` (this repo) | Full timetable | every 6h |
| `path_schedule.h` | Baked-in fallback | build time |

The ESP32 fetches `schedule.json` from this repo's `main` branch. A GitHub
Action refreshes it daily. If the fetch fails, the firmware falls back to
the schedule compiled into `src/path_schedule.h`.

## Refreshing the schedule

PATH updates its timetable every ~2-3 weeks. The GitHub Action runs daily
and commits `schedule.json` if it changed. To refresh manually:

```sh
python3 tools/extract_schedule.py schedule.json
git add schedule.json && git commit -m "Update schedule" && git push
```

> Use the **system** `python3` (not PlatformIO's) — it has the CA certs
> needed for the HTTPS download.

## Configuration

All the tunables are `#define`s at the top of `src/main.cpp`:

| Define | Default | Purpose |
|--------|---------|---------|
| `SHOW_CLOCK_HANDS` | `true` | Show hour/minute/second hands |
| `MAX_TRAINS` | `6` | How many upcoming trains to show |
| `USE_SCHEDULE` | `true` | Fill in trains beyond the live 2 |
| `DEBUG_LOG` | `true` | Verbose serial logging |
| `FORCE_ALERT` | `false` | Test: force the alert display on |
| `FETCH_INTERVAL_MS` | `30000` | Live train poll interval |
| `ALERTS_INTERVAL_MS` | `120000` | Alert poll interval |
| `SCHEDULE_INTERVAL_MS` | `6h` | Schedule refresh interval |

## Viewing logs

The firmware logs to serial at **115200 baud**. To watch it live:

```sh
pio device monitor
```

Or read it directly (works when `pio device monitor` complains about TTY):

```sh
python3 -c "
import serial, time
s = serial.Serial('/dev/cu.usbserial-0001', 115200, timeout=1)
while True:
    line = s.readline()
    if line: print(line.decode(errors='replace').rstrip())
"
```

Set `DEBUG_LOG false` to silence the per-fetch output (boot and error
messages still print).

## Hardware

- ESP32 Dev Module (CP2102)
- 60x WS2812B ring, external 5V supply, shared ground
- Data: **D5 / GPIO 5** (through a 330Ω resistor)

## Build & flash

```sh
pio run --target upload
```

WiFi credentials go in `src/secrets.h` (gitignored).
