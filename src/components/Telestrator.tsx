import React, { useCallback, useEffect, useRef } from 'react';

/**
 * NFL-instant-replay style telestrator: a transparent canvas overlay that lets
 * a commentator scribble in red over the frozen / playing replay board. Purely
 * client-side and purely additive — it sits absolutely on top of its parent
 * (which must be `position: relative/absolute`) and only intercepts pointer
 * events while `active`. When inactive it is `pointer-events: none`, so the
 * replay scrubs / seeks exactly as before.
 *
 * Drawing itself lives here; the ENTER / EXIT (+ clear) toggle is owned by the
 * parent so the same hotkey and on-screen button can drive it. Each time the
 * overlay becomes active it mounts a blank canvas, and the parent clears it on
 * exit by flipping `active` off — so every entry starts fresh.
 */

const STROKE = '#ff1414';
const STROKE_WIDTH = 5;
// a soft dark halo keeps the red pen legible over light/themed backgrounds
const SHADOW = 'rgba(0,0,0,0.55)';
const SHADOW_BLUR = 3;

export const Telestrator = ({ active }: { active: boolean }) => {
    const canvasRef = useRef<HTMLCanvasElement | null>(null);
    const drawing = useRef(false);
    const last = useRef<{ x: number; y: number } | null>(null);

    // Keep the canvas backing store matched to its CSS pixel size (and to the
    // device pixel ratio) so strokes stay crisp and aligned with the board.
    const resize = useCallback(() => {
        const canvas = canvasRef.current;
        if (!canvas) return;
        const dpr = window.devicePixelRatio || 1;
        const rect = canvas.getBoundingClientRect();
        // Resizing the backing store wipes it, so snapshot and restore the
        // existing drawing across a resize.
        const prev = document.createElement('canvas');
        prev.width = canvas.width;
        prev.height = canvas.height;
        if (canvas.width && canvas.height) {
            prev.getContext('2d')?.drawImage(canvas, 0, 0);
        }
        canvas.width = Math.max(1, Math.round(rect.width * dpr));
        canvas.height = Math.max(1, Math.round(rect.height * dpr));
        const ctx = canvas.getContext('2d');
        if (!ctx) return;
        ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
        ctx.lineCap = 'round';
        ctx.lineJoin = 'round';
        ctx.strokeStyle = STROKE;
        ctx.lineWidth = STROKE_WIDTH;
        ctx.shadowColor = SHADOW;
        ctx.shadowBlur = SHADOW_BLUR;
        if (prev.width && prev.height) {
            ctx.save();
            ctx.setTransform(1, 0, 0, 1, 0, 0);
            ctx.drawImage(prev, 0, 0);
            ctx.restore();
        }
    }, []);

    useEffect(() => {
        if (!active) return;
        resize();
        window.addEventListener('resize', resize);
        return () => window.removeEventListener('resize', resize);
    }, [active, resize]);

    const pointFor = (canvas: HTMLCanvasElement, e: React.PointerEvent) => {
        const rect = canvas.getBoundingClientRect();
        return { x: e.clientX - rect.left, y: e.clientY - rect.top };
    };

    const onPointerDown = (e: React.PointerEvent<HTMLCanvasElement>) => {
        const canvas = canvasRef.current;
        if (!canvas) return;
        e.preventDefault();
        canvas.setPointerCapture(e.pointerId);
        drawing.current = true;
        const p = pointFor(canvas, e);
        last.current = p;
        // a single tap leaves a dot
        const ctx = canvas.getContext('2d');
        if (ctx) {
            ctx.beginPath();
            ctx.moveTo(p.x, p.y);
            ctx.lineTo(p.x + 0.01, p.y + 0.01);
            ctx.stroke();
        }
    };

    const onPointerMove = (e: React.PointerEvent<HTMLCanvasElement>) => {
        if (!drawing.current) return;
        const canvas = canvasRef.current;
        if (!canvas) return;
        e.preventDefault();
        const ctx = canvas.getContext('2d');
        if (!ctx || !last.current) return;
        const p = pointFor(canvas, e);
        ctx.beginPath();
        ctx.moveTo(last.current.x, last.current.y);
        ctx.lineTo(p.x, p.y);
        ctx.stroke();
        last.current = p;
    };

    const endStroke = (e: React.PointerEvent<HTMLCanvasElement>) => {
        const canvas = canvasRef.current;
        if (canvas && canvas.hasPointerCapture(e.pointerId)) {
            canvas.releasePointerCapture(e.pointerId);
        }
        drawing.current = false;
        last.current = null;
    };

    if (!active) return null;

    return (
        <canvas
            ref={canvasRef}
            onPointerDown={onPointerDown}
            onPointerMove={onPointerMove}
            onPointerUp={endStroke}
            onPointerCancel={endStroke}
            onPointerLeave={endStroke}
            style={{
                position: 'absolute',
                inset: 0,
                width: '100%',
                height: '100%',
                zIndex: 1200, // above board + transport controls, below modals
                cursor: 'crosshair',
                touchAction: 'none', // own touch gestures so we draw, not scroll
            }}
        />
    );
};
