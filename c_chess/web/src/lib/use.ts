'use client';
// use.ts - the store (net.ts) as React state.
import { useSyncExternalStore } from 'react';
import { net, type Snapshot } from './net';

const SERVER: Snapshot = { phase: 'loading', view: null, moves: null, lobby: null, events: 0 };

export function useNet(): Snapshot {
  return useSyncExternalStore(net.subscribe, net.get, () => SERVER);
}

/** "3+2" */
export const tc = (base: number, inc: number): string => `${base % 60 === 0 ? base / 60 : (base / 60).toFixed(1).replace(/\.0$/, '')}+${inc}`;
