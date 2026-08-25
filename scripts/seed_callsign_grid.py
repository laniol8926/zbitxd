#!/usr/bin/env python3
"""One-time bulk seed of the callsign_grid sqlite table (see
callsign_grid_ensure_table(), logbook.c) from the FCC ULS amateur
radio database. Downloads the FCC's own callsign/zip data plus a
Census Bureau ZIP-code centroid reference, converts each licensee's
zip to a 4-character Maidenhead grid square, and bulk-inserts the
result via the sqlite3 CLI (matching this repo's own existing
convention for seeding sbitx.db, see the Makefile's create_db.sql
step).

Run once, normally from install.sh:
    python3 scripts/seed_callsign_grid.py /var/lib/zbitxd/sbitx.db

Field positions below were confirmed directly against real downloaded
data (2026-08-25), not just FCC's own layout doc, since that doc's
numbering has changed before.
"""

import csv
import subprocess
import sys
import time
import zipfile
from io import BytesIO, TextIOWrapper

FCC_ULS_URL = "https://data.fcc.gov/download/pub/uls/complete/l_amat.zip"
# Census re-publishes this under a new year's path periodically -- if a
# future run 404s, check https://www2.census.gov/geo/docs/maps-data/data/gazetteer/
# for the current year and update this URL.
GAZETTEER_URL = "https://www2.census.gov/geo/docs/maps-data/data/gazetteer/2025_Gazetteer/2025_Gaz_zcta_national.zip"

USER_AGENT = "Mozilla/5.0"


def fetch_zip(url):
	# Shelling out to curl rather than urllib -- confirmed live (2026-08-25)
	# that data.fcc.gov returns HTTP 403 to Python's urllib specifically
	# (client/TLS fingerprinting, not IP-based -- the exact same request,
	# same User-Agent header, succeeds via curl from the same machine).
	# curl is a declared install.sh dependency for exactly this reason.
	result = subprocess.run(
		["curl", "-sL", "-A", USER_AGENT, "--max-time", "300", url],
		stdout=subprocess.PIPE, check=True)
	return zipfile.ZipFile(BytesIO(result.stdout))


def load_zip_centroids():
	"""zip (5-digit str) -> (lat, lon)."""
	print("Downloading Census Gazetteer ZCTA centroids...", file=sys.stderr)
	zf = fetch_zip(GAZETTEER_URL)
	name = [n for n in zf.namelist() if n.endswith(".txt")][0]
	centroids = {}
	with zf.open(name) as raw:
		text = TextIOWrapper(raw, encoding="latin-1")
		reader = csv.reader(text, delimiter="|")
		header = next(reader)
		geoid_i = header.index("GEOID")
		lat_i = header.index("INTPTLAT")
		lon_i = header.index("INTPTLONG")
		for row in reader:
			if len(row) <= max(geoid_i, lat_i, lon_i):
				continue
			zip5 = row[geoid_i].strip()
			try:
				centroids[zip5] = (float(row[lat_i]), float(row[lon_i]))
			except ValueError:
				continue
	print(f"  {len(centroids)} ZCTA centroids loaded.", file=sys.stderr)
	return centroids


def latlon_to_grid(lat, lon):
	"""Standard 4-character Maidenhead locator."""
	lon_adj = lon + 180.0
	lat_adj = lat + 90.0
	field1 = chr(ord('A') + int(lon_adj / 20))
	field2 = chr(ord('A') + int(lat_adj / 10))
	square1 = int((lon_adj % 20) / 2)
	square2 = int(lat_adj % 10)
	return f"{field1}{field2}{square1}{square2}"


def is_plain_callsign(callsign):
	# FCC callsigns are always plain alphanumeric -- reject anything
	# else defensively rather than trust external data blindly (also
	# doubles as SQL-injection-safe, since callsigns get interpolated
	# directly into the generated SQL rather than bound as parameters --
	# see the sqlite3-CLI-batching comment below for why).
	return callsign.isalnum()


def seed_callsign_grid(db_path):
	centroids = load_zip_centroids()

	print("Downloading FCC ULS amateur database...", file=sys.stderr)
	zf = fetch_zip(FCC_ULS_URL)
	now = int(time.time())

	print("Parsing EN.dat and writing to sbitx.db...", file=sys.stderr)
	# INSERT OR IGNORE, not the callsign_grid_set() upsert -- a
	# 'decode'-sourced row already present by the time this seed runs
	# (e.g. a re-run after the daemon's been live a while) must never
	# be clobbered by a mailing-address-derived guess.
	proc = subprocess.Popen(
		["sqlite3", db_path],
		stdin=subprocess.PIPE, text=True)
	proc.stdin.write("BEGIN;\n")

	written = 0
	batch = []
	BATCH_SIZE = 500
	with zf.open("EN.dat") as raw:
		text = TextIOWrapper(raw, encoding="latin-1")
		reader = csv.reader(text, delimiter="|")
		for row in reader:
			# EN.dat has no header row -- field indices confirmed
			# directly against real downloaded data:
			# [4]=Call Sign [5]=Entity Type [18]=Zip Code
			if len(row) <= 18:
				continue
			callsign = row[4].strip().upper()
			entity_type = row[5].strip()
			zip_raw = row[18].strip()
			if entity_type != "L" or not callsign or not is_plain_callsign(callsign):
				continue
			zip5 = zip_raw[:5]
			if len(zip5) != 5 or not zip5.isdigit():
				continue
			latlon = centroids.get(zip5)
			if not latlon:
				continue
			grid = latlon_to_grid(*latlon)
			batch.append(f"('{callsign}','{grid}','uls',{now})")
			if len(batch) >= BATCH_SIZE:
				proc.stdin.write(
					"INSERT OR IGNORE INTO callsign_grid (callsign, grid, source, updated_at) VALUES "
					+ ",".join(batch) + ";\n")
				written += len(batch)
				batch = []

	if batch:
		proc.stdin.write(
			"INSERT OR IGNORE INTO callsign_grid (callsign, grid, source, updated_at) VALUES "
			+ ",".join(batch) + ";\n")
		written += len(batch)

	proc.stdin.write("COMMIT;\n")
	proc.stdin.close()
	proc.wait()
	print(f"Seeded {written} callsign->grid rows (source='uls').", file=sys.stderr)


if __name__ == "__main__":
	if len(sys.argv) != 2:
		print(f"usage: {sys.argv[0]} <path-to-sbitx.db>", file=sys.stderr)
		sys.exit(1)
	seed_callsign_grid(sys.argv[1])
