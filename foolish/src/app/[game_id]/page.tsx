import { KernelGate } from '../../components/KernelGate';
import { GAME_SHELL_PARAM } from '../../utils/gameShell';
import { GameRoute } from './GameRoute';

// One prerendered page for every game id and replay code: the static export
// writes out/_shell.html and the edge serves it for any path that is not a file
// or a known page; the app reads the address (src/utils/gameShell.ts).
// A server build still renders other ids on demand.
export function generateStaticParams() {
  return [{ game_id: GAME_SHELL_PARAM }];
}

export default function GamePage() {
  return (
    <KernelGate link="address">
      <GameRoute />
    </KernelGate>
  );
}
