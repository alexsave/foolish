'use client';
// use.ts - the store (net.ts) as React state.
import { useSyncExternalStore } from 'react';
import { net, type Snapshot } from './net';

const SERVER: Snapshot = { phase: 'loading', view: null, moves: null, lobby: null, tv: null, eval: null, events: 0 };

export function useNet(): Snapshot {
  return useSyncExternalStore(net.subscribe, net.get, () => SERVER);
}

/** "3+2", "½+0" (lichess's quarter minutes), "1.5+0", "10s+5" */
const QUARTERS: Readonly<Record<number, string>> = { 15: '¼', 30: '½', 45: '¾' };
const minutes = (base: number): string =>
  base % 60 === 0 ? `${base / 60}` : QUARTERS[base] ?? (base < 60 ? `${base}s` : (base / 60).toFixed(1).replace(/\.0$/, ''));
export const tc = (base: number, inc: number): string => `${minutes(base)}+${inc}`;
