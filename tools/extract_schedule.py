#!/usr/bin/env python3
"""
Extract the PATH schedule for Grove Street -> 33rd Street (ToNY) from the
RidePATH GTFS database and write a compact JSON file for the ESP32.

Usage:
    python3 tools/extract_schedule.py [output.json]

The script:
  1. Downloads the RidePATH SQLite database (3.4MB zip -> 19MB db)
  2. Extracts YEL + ATW departures from Grove St toward 33rd Street
  3. Writes a small JSON file (~2KB) with weekday/saturday/sunday times

Run this whenever PATH updates its timetable (roughly every 2-3 weeks).
"""

import io
import json
import sqlite3
import sys
import urllib.request
import zipfile

# --- PATH API ---------------------------------------------------------------
API_URL = "https://path-mppprod-app.azurewebsites.net/api/v3/file/datafile"
CHECKSUM = "4BC58453B4B6F272AF23975BF9939704"
HEADERS = {
    "AppName": "RidePATH",
    "AppVersion": "6.2.2",
    "Platform": "iOS",
    "Content-Type": "application/json; charset=utf-8",
    "User-Agent": "RidePATH/6.2.2.23938 CFNetwork/3860.700.1 Darwin/25.6.0",
    "AuthKey": "a9fc79c6-a434-4124-a7d6-6b3008105bf9",
    "AppInstallId": "E1B7E3FD-5D47-4D05-94AB-E8286FBFFD80",
}
BODY = json.dumps({"checksum": CHECKSUM, "type": "database"}).encode()

# --- GTFS identifiers -------------------------------------------------------
GROVE_ST_TO_NY = "781726"   # Grove St, ToNY direction (Grove before 33rd)
ROUTES = ("YEL", "ATW")     # Journal Square + via Hoboken (both to 33rd St)

# Service IDs are date-ranged; pick the one active today. We query the
# calendar table and choose the service whose range covers today.
DAY_COLUMNS = {
    "weekday": ("monday", "tuesday", "wednesday", "thursday", "friday"),
    "saturday": ("saturday",),
    "sunday": ("sunday",),
}


def download_db() -> bytes:
    print("Downloading RidePATH database...")
    req = urllib.request.Request(API_URL, data=BODY, headers=HEADERS, method="POST")
    with urllib.request.urlopen(req, timeout=120) as resp:
        data = resp.read()
    print(f"  downloaded {len(data):,} bytes")
    return data


def open_db(zip_bytes: bytes) -> sqlite3.Connection:
    with zipfile.ZipFile(io.BytesIO(zip_bytes)) as z:
        name = z.namelist()[0]
        print(f"  extracting {name}")
        db_bytes = z.read(name)
    # SQLite needs a real file; write to a temp path.
    import tempfile, os
    fd, path = tempfile.mkstemp(suffix=".db")
    with os.fdopen(fd, "wb") as f:
        f.write(db_bytes)
    return sqlite3.connect(path), path


def active_service(cur, day_cols, today_yyyymmdd: str):
    """Return the service_id for the given day type.

    Looks for a service active today; if none (e.g. asking for Sunday on a
    Saturday), finds the next upcoming date that the service covers.
    """
    where = " OR ".join(f"{c}=1" for c in day_cols)

    # Prefer a service active today.
    cur.execute(
        f"""SELECT service_id FROM 'gtfs.base.calendar'
            WHERE ({where}) AND start_date <= ? AND end_date >= ?
            ORDER BY start_date DESC LIMIT 1""",
        (today_yyyymmdd, today_yyyymmdd),
    )
    row = cur.fetchone()
    if row:
        return row[0]

    # Otherwise, the soonest-starting service of this day type.
    cur.execute(
        f"""SELECT service_id FROM 'gtfs.base.calendar'
            WHERE ({where}) AND end_date >= ?
            ORDER BY start_date ASC LIMIT 1""",
        (today_yyyymmdd,),
    )
    row = cur.fetchone()
    return row[0] if row else None


def departures(cur, service_id):
    cur.execute(
        """SELECT st.departure_time, t.route_id
           FROM 'gtfs.base.stop_times' st
           JOIN 'gtfs.base.trips' t ON st.trip_id = t.trip_id
           WHERE st.stop_id = ? AND t.service_id = ?
             AND t.route_id IN (?, ?)
           ORDER BY st.departure_time""",
        (GROVE_ST_TO_NY, service_id, *ROUTES),
    )
    out = []
    for dep, route in cur.fetchall():
        h, m, _ = dep.split(":")
        out.append([int(h) * 60 + int(m), 0 if route == "YEL" else 1])
    return out


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "schedule.json"

    zip_bytes = download_db()
    conn, db_path = open_db(zip_bytes)
    cur = conn.cursor()

    # Use a representative "today" so we pick the currently-active service.
    import datetime
    today = datetime.date.today().strftime("%Y%m%d")

    result = {"generated": datetime.datetime.utcnow().isoformat() + "Z"}
    for day, cols in DAY_COLUMNS.items():
        svc = active_service(cur, cols, today)
        if not svc:
            print(f"  WARNING: no active {day} service found")
            result[day] = []
            continue
        deps = departures(cur, svc)
        result[day] = deps
        print(f"  {day}: {len(deps)} trains (service {svc})")

    conn.close()
    import os
    os.unlink(db_path)

    with open(out_path, "w") as f:
        json.dump(result, f, separators=(",", ":"))
    print(f"Wrote {out_path} ({os.path.getsize(out_path):,} bytes)")


if __name__ == "__main__":
    main()
