import { createContext, useContext } from 'react';
import { Card } from '@shared/core/types.ts';

/* An optional, tutorial-only hint that the live ActionButtons reads to glow the
 * card(s) and the wooden button the learner should use next. It defaults to
 * null, so the real game renders exactly as before. */
export interface TutorialHint {
    /** cards in the learner's hand to highlight green */
    cards: Card[];
    /** which action button to highlight: attack | cover | pass | pickup | good */
    action: string | null;
    /** the table attack card to highlight as a cover/drag target, if any */
    targetCard?: Card | null;
}

const TutorialHintContext = createContext<TutorialHint | null>(null);

export const TutorialHintProvider = TutorialHintContext.Provider;

export const useTutorialHint = (): TutorialHint | null => useContext(TutorialHintContext);
