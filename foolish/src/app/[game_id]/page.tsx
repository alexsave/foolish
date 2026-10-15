import { KernelGate } from '../../components/KernelGate';
import { GAME_SHELL_PARAM } from '../../utils/gameShell';
import { GameRoute } from './GameRoute';

// One prerendered page for every game id and replay code: the static export
// writes out/_shell.html and CloudFront serves it for any unknown one-segment
// path (src/utils/gameShell.ts). A server build still renders other ids on demand.
export function generateStaticParams() {
  return [{ game_id: GAME_SHELL_PARAM }];
}

export default function GamePage() {
  return (
    <KernelGate>
      <GameRoute />
    </KernelGate>
  );
}
