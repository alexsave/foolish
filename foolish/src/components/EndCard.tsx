import React from 'react';

/* The card a self-contained game ends on, over its final board: what happened,
 * then what to do next. The tutorial's and the promo game's (PromoGame.tsx). */
export const EndCard = ({ testId, children, actions }: {
    testId: string;
    /** What happened: a line or two. */
    children: React.ReactNode;
    /** The buttons. */
    actions: React.ReactNode;
}) => (
    <div
        data-testid={testId}
        style={{ position: 'absolute', inset: 0, zIndex: 1300, display: 'flex', alignItems: 'center', justifyContent: 'center', background: 'rgba(8,6,4,0.55)' }}
    >
        <div
            className="text-shadow"
            style={{
                width: 'min(90vw, 440px)', textAlign: 'center', background: 'rgba(24,18,12,0.92)',
                border: '1px solid rgba(231,151,67,0.4)', borderRadius: 16, padding: '24px 22px',
                color: 'var(--color-text-primary)', boxShadow: '0 10px 40px rgba(0,0,0,0.6)',
            }}
        >
            <div style={{ fontSize: '2rem', marginBottom: 6 }}>🃏</div>
            {children}
            <div style={{ display: 'flex', gap: 10, justifyContent: 'center', flexWrap: 'wrap' }}>{actions}</div>
        </div>
    </div>
);
