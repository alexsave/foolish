-- c_chess's tables, after the platform's (c_base/providers/local/sql/schema.sql), applied at every
-- open: the journal behind the in-memory state (server/src/journal.c).
CREATE TABLE IF NOT EXISTS cc_players (
    id BLOB PRIMARY KEY,           -- the account's uuid, 16 bytes
    name TEXT NOT NULL,
    games INTEGER NOT NULL, wins INTEGER NOT NULL, losses INTEGER NOT NULL, draws INTEGER NOT NULL,
    last_at INTEGER NOT NULL,
    pr REAL NOT NULL, pd REAL NOT NULL, pv REAL NOT NULL,   -- the puzzle rating
    pz_tried INTEGER NOT NULL, pz_solved INTEGER NOT NULL
);
-- a player's rating of a speed (c/src/speed.h: 0 bullet, 1 blitz, 2 rapid, 3 classical), written
-- after each rated game of it; a speed never rated has no row (the start rating)
CREATE TABLE IF NOT EXISTS cc_ratings (
    player BLOB NOT NULL, speed INTEGER NOT NULL,
    r REAL NOT NULL, d REAL NOT NULL, v REAL NOT NULL,
    games INTEGER NOT NULL,        -- rated games of the speed
    PRIMARY KEY (player, speed)
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS cc_games (
    id INTEGER PRIMARY KEY,
    white BLOB NOT NULL, black BLOB NOT NULL,
    base_s INTEGER NOT NULL, inc_s INTEGER NOT NULL, rated INTEGER NOT NULL,
    status INTEGER NOT NULL, winner INTEGER NOT NULL,
    started_at INTEGER NOT NULL, ended_at INTEGER NOT NULL,
    r_w INTEGER NOT NULL, r_b INTEGER NOT NULL, d_w INTEGER NOT NULL, d_b INTEGER NOT NULL,
    left_w INTEGER NOT NULL DEFAULT 0, left_b INTEGER NOT NULL DEFAULT 0,   -- the clocks at the end, ms
    moves BLOB                     -- u16 per ply, little-endian; NULL while the game is in play
);
CREATE INDEX IF NOT EXISTS cc_games_white ON cc_games (white, id);
CREATE INDEX IF NOT EXISTS cc_games_black ON cc_games (black, id);
CREATE INDEX IF NOT EXISTS cc_games_playing ON cc_games (status) WHERE status = 1;
-- the moves of games in play are not here: they are appended to a flat file beside the database
-- (journal.c, "<db>-moves"), and a finished game's moves land in cc_games.moves
-- a rating after each rated game, of the game's speed; a profile reads the latest of each speed
CREATE TABLE IF NOT EXISTS cc_points (
    player BLOB NOT NULL, speed INTEGER NOT NULL, at INTEGER NOT NULL, rating INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS cc_points_player ON cc_points (player, speed, at);
-- a puzzle's rating as people solve it (the puzzles themselves are a file: CC_PUZZLES, puzzle.h);
-- the id is the record's (puzzles.c: a hash of its position and line, so a regenerated file keeps
-- the ratings of the puzzles it kept)
CREATE TABLE IF NOT EXISTS cc_puzzles (
    id INTEGER PRIMARY KEY, r REAL NOT NULL, d REAL NOT NULL, v REAL NOT NULL, plays INTEGER NOT NULL
);
