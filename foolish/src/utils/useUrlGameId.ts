import { useEffect, useState } from 'react';
import { useParams, usePathname } from 'next/navigation';

import { GAME_SHELL_PARAM, shellSegment } from './gameShell';

/**
 * The /[game_id] path segment, exactly as typed (callers lowercase game ids;
 * replay codes keep their case). `undefined` off the game route, and also for
 * the static shell's first render, before the address bar has been read.
 *
 * The one place the web reads the segment: every reader used to call
 * useParams() itself, and on the static export each would have read the
 * placeholder.
 *
 * null when the shell was served for an address that is no game path (two or
 * more segments: shellSegment), which the game route answers with not-found.
 */
export function useUrlGameId(): string | null | undefined {
    const param = useParams<{ game_id?: string }>().game_id;
    const pathname = usePathname();
    const [fromAddress, setFromAddress] = useState<string | null | undefined>(undefined);
    useEffect(() => {
        if (param !== GAME_SHELL_PARAM) return;
        setFromAddress(shellSegment(window.location.pathname));
    }, [param, pathname]);
    if (param === undefined) return undefined;
    return param === GAME_SHELL_PARAM ? fromAddress : param;
}
