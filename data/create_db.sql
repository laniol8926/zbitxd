create table messages (
	id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,
	mode TEXT,
	freq INTEGER,
	qso_date INTEGER,
	qso_time INTEGER,
	is_outgoing INTEGER DEFAULT 0,	
	data TEXT DEFAULT ""
);

create table contacts(
	id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,
	callsign TEXT,
	name TEXT,
	last_seen_on INTEGER,
	status TEXT
);


create table logbook (
	id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,
	mode TEXT,
	freq TEXT,
	qso_date TEXT,
	qso_time TEXT,
	callsign_sent TEXT,
	rst_sent TEXT,
	exch_sent TEXT DEFAULT "",
	callsign_recv TEXT,
	rst_recv TEXT,
	exch_recv TEXT DEFAULT "",
	tx_id	TEXT DEFAULT "",
	comments TEXT DEFAULT ""
);
CREATE INDEX gridIx ON logbook (exch_recv);
CREATE INDEX callIx ON logbook (callsign_recv);

-- WSJT-X-style per-band/per-mode dial frequency table (Settings > Frequencies
-- there). One row per (band, mode); band_freq_ensure_table() in logbook.c
-- seeds the standard FT8/FT4 calling frequencies on first run and also
-- creates this table itself (CREATE TABLE IF NOT EXISTS) so an existing,
-- already-deployed sbitx.db picks it up too -- this file only runs on a
-- brand new database, never against one that already exists.
create table band_frequencies (
	id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,
	band TEXT NOT NULL,
	mode TEXT NOT NULL,
	freq INTEGER NOT NULL,
	UNIQUE(band, mode)
);

