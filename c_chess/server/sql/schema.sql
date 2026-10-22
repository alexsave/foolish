-- c_chess's tables, after the platform's (c_base/providers/local/sql/schema.sql), applied at every
-- open: the journal behind the in-memory state (server/src/journal.c).
CREATE TABLE IF NOT EXISTS cc_players (
    id BLOB PRIMARY KEY,           -- the account's uuid, 16 bytes
    name TEXT NOT NULL,
    r REAL NOT NULL, d REAL NOT NULL, v REAL NOT NULL,
    games INTEGER NOT NULL, wins INTEGER NOT NULL, losses INTEGER NOT NULL, draws INTEGER NOT NULL,
    last_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS cc_games (
    id INTEGER PRIMARY KEY,
    white BLOB NOT NULL, black BLOB NOT NULL,
    base_s INTEGER NOT NULL, inc_s INTEGER NOT NULL, rated INTEGER NOT NULL,
    status INTEGER NOT NULL, winner INTEGER NOT NULL,
    started_at INTEGER NOT NULL, ended_at INTEGER NOT NULL,
    r_w INTEGER NOT NULL, r_b INTEGER NOT NULL, d_w INTEGER NOT NULL, d_b INTEGER NOT NULL,
    moves BLOB                     -- u16 per ply, little-endian; NULL while the game is in play
);
CREATE INDEX IF NOT EXISTS cc_games_white ON cc_games (white, id);
CREATE INDEX IF NOT EXISTS cc_games_black ON cc_games (black, id);
CREATE INDEX IF NOT EXISTS cc_games_playing ON cc_games (status) WHERE status = 1;
-- the moves of games in play (a finished game's are in cc_games.moves and these rows are deleted)
CREATE TABLE IF NOT EXISTS cc_moves (
    game INTEGER NOT NULL, ply INTEGER NOT NULL, move INTEGER NOT NULL, left_ms INTEGER NOT NULL,
    PRIMARY KEY (game, ply)
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS cc_points (
    player BLOB NOT NULL, at INTEGER NOT NULL, rating INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS cc_points_player ON cc_points (player, at);
