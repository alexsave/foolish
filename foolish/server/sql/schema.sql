-- foolish's tables on the SQL store (c_base/core/src/store/store_sql.h), applied after
-- c_base/providers/local/sql/schema.sql in the same transaction at every open: every
-- statement is idempotent. The rows are written by foolish/server/src/store/rows_sql.c.
--
-- A UUID (a user, a bot, a replay, a chat message) is its 16 bytes. A game id is the
-- game's short code itself (TEXT: the same code the share URL and its QR code carry).
-- WITHOUT ROWID where the primary key is the access path and the row is small; a table
-- whose rows are KBs (games, replays) keeps its rowid, so a row stays on one leaf page
-- (a WITHOUT ROWID row over about 1 KB spills to overflow pages, rewritten with it).

-- A game: the kernel's state and roster and the scalar columns, a fixed size. The
-- session log is NOT here: it is game_moves. log_from is the seq of the current
-- session's first move (0: none). updated_at is the last commit's time.
CREATE TABLE IF NOT EXISTS games (
    id             TEXT PRIMARY KEY,
    status         INTEGER NOT NULL,
    version        INTEGER NOT NULL,
    state          BLOB NOT NULL,
    roster         BLOB NOT NULL,
    seed           BLOB,
    needs_bots     INTEGER NOT NULL,
    round_epoch    INTEGER NOT NULL,
    log_from       INTEGER NOT NULL,
    created_at     INTEGER NOT NULL,
    updated_at     INTEGER NOT NULL,
    last_commit_at INTEGER NOT NULL
);

-- The append-only session log: one row per commit that appended kernel log records,
-- at seq = the version that commit wrote. A move appends one row; a deal or a lobby
-- reset deletes the game's rows; the bot drive and the replay at game end read them
-- in order from log_from.
CREATE TABLE IF NOT EXISTS game_moves (
    game_id TEXT NOT NULL,
    seq     INTEGER NOT NULL,
    action  BLOB NOT NULL,
    PRIMARY KEY (game_id, seq)
) WITHOUT ROWID;

-- The humans seated at a game (the dashboard, the chat fan-out, membership checks).
CREATE TABLE IF NOT EXISTS game_members (
    game_id   TEXT NOT NULL,
    user_id   BLOB NOT NULL,
    joined_at INTEGER NOT NULL,
    PRIMARY KEY (game_id, user_id)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS game_members_user ON game_members(user_id);

CREATE TABLE IF NOT EXISTS game_bots (
    game_id TEXT NOT NULL,
    bot_id  BLOB NOT NULL,
    PRIMARY KEY (game_id, bot_id)
) WITHOUT ROWID;

-- previous_elo -1: none.
CREATE TABLE IF NOT EXISTS bots (
    id           BLOB PRIMARY KEY,
    nickname     TEXT NOT NULL,
    strategy_key TEXT NOT NULL,
    elo_rating   INTEGER NOT NULL,
    previous_elo INTEGER NOT NULL,
    games_played INTEGER NOT NULL,
    created_at   INTEGER NOT NULL,
    updated_at   INTEGER NOT NULL
) WITHOUT ROWID;

-- A user's rating. The leaderboard reads the played ones, best first.
CREATE TABLE IF NOT EXISTS ratings (
    user_id      BLOB PRIMARY KEY,
    username     TEXT,
    elo_rating   INTEGER NOT NULL,
    previous_elo INTEGER NOT NULL,
    games_played INTEGER NOT NULL,
    updated_at   INTEGER NOT NULL
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS ratings_top ON ratings(elo_rating DESC, user_id) WHERE games_played > 0;

-- A finished game's replay: the kernel's v6 replay code (moves), the extras (the seats'
-- names and gaps), the players in seat order (16 bytes each, bots included), and the
-- summary the history list shows (fool, dur; NULL: none). game_id NULL once the game
-- is deleted. It keeps its own encoded moves rather than pointing at game_moves: the
-- code is a different, compact format, and a replay outlives both the game and its
-- session log (a rematch's deal clears the log).
CREATE TABLE IF NOT EXISTS replays (
    id         BLOB PRIMARY KEY,
    game_id    TEXT,
    players    BLOB NOT NULL,
    moves      BLOB NOT NULL,
    extras     BLOB,
    created_at INTEGER NOT NULL,
    fool       INTEGER,
    dur        INTEGER
);
CREATE INDEX IF NOT EXISTS replays_game ON replays(game_id) WHERE game_id IS NOT NULL;

-- A player's history: one row per player of a replay, by date (h_history, share links).
-- rank NULL when the replay has no summary.
CREATE TABLE IF NOT EXISTS replay_players (
    user_id    BLOB NOT NULL,
    created_at INTEGER NOT NULL,
    replay_id  BLOB NOT NULL,
    seat       INTEGER NOT NULL,
    rank       INTEGER,
    PRIMARY KEY (user_id, created_at, replay_id)
) WITHOUT ROWID;

-- A game's chat, by date.
CREATE TABLE IF NOT EXISTS chat (
    game_id    TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    id         BLOB NOT NULL,
    user_id    BLOB NOT NULL,
    message    TEXT NOT NULL,
    is_system  INTEGER NOT NULL,
    PRIMARY KEY (game_id, created_at, id)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS chat_user ON chat(user_id);
