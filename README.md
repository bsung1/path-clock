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
  - The **next train** flashes, speeding up as it approaches, then turns
    solid red under 4 minutes.

## Data sources

| Source | What | Frequency |
|--------|------|-----------|
| `ridepath.json` | Live next-2 trains | every 30s |
| `schedule.json` (this repo) | Full timetable | every 6h |
| `path_schedule.h` | Baked-in fallback | build time |

The ESP32 fetches `schedule.json` from this repo's `main` branch. A GitHub
Action refreshes it weekly. If the fetch fails, the firmware falls back to
the schedule compiled into `src/path_schedule.h`.

## Refreshing the schedule

PATH updates its timetable every ~2-3 weeks. The GitHub Action runs weekly
and commits `schedule.json` if it changed. To refresh manually:

```sh
python3 tools/extract_schedule.py schedule.json
git add schedule.json && git commit -m "Update schedule" && git push
```

## Hardware

- ESP32 Dev Module (CP2102)
- 60x WS2812B ring, external 5V supply, shared ground
- Data: **D5 / GPIO 5** (through a 330Ω resistor)

## Build & flash

```sh
pio run --target upload
```

WiFi credentials go in `src/secrets.h` (gitignored).
