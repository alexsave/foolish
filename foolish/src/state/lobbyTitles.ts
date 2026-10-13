import { useSyncExternalStore } from 'react';
import { GAME_STATUS, type TableView } from './view';

// The names of the lobbies this tab has seen, by game id, kept across a page
// navigation.
//
// ServerProvider is mounted per route (ProtectedRoute), so the boards it holds
// are gone the moment the dashboard hands over to /[game_id], and the game page
// starts with nothing while it reads its view. The one thing worth drawing in
// that gap is the game's name, and the tab already knew it: the dashboard listed
// the table, or `create` just answered with it. This is where that name waits.
//
// It holds lobbies only. The name is the top line of the lobby and of no other
// screen, so a held name for a game that is being played or is over would be
// drawn and then taken away again when the board lands.
//
// A page opened straight on /[game_id] has seen nothing, and gets nothing.
const titles = new Map<string, string>();
const listeners = new Set<() => void>();

/** Every board the page holds, as it holds them: ServerProvider is the one caller. */
export function holdLobbyTitles(views: Iterable<TableView>): void {
    let changed = false;
    for (const view of views) {
        const title = view.status === GAME_STATUS.WAITING ? view.title : undefined;
        if (titles.get(view.gameId) === title) continue;
        if (title === undefined) titles.delete(view.gameId); else titles.set(view.gameId, title);
        changed = true;
    }
    if (changed) listeners.forEach((listener) => listener());
}

const subscribe = (listener: () => void) => {
    listeners.add(listener);
    return () => { listeners.delete(listener); };
};

/** The held name of the lobby `gameId`, or null: followed, so a name that lands while a screen is up is drawn. */
export function useLobbyTitle(gameId: string | null): string | null {
    const read = () => (gameId ? titles.get(gameId) ?? null : null);
    return useSyncExternalStore(subscribe, read, () => null);
}
