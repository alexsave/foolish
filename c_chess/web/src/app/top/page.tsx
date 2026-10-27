'use client';
// The leaderboards, one a speed (?speed=bullet|blitz|rapid|classical, blitz when none).
import { Suspense } from 'react';
import { useRouter, useSearchParams } from 'next/navigation';
import Leaders from '@/components/Leaders';
import { CC_BLITZ } from '@/gen/web_layout';
import { SPEEDS, SPEED_NAME } from '@/lib/words';

function Boards() {
  const router = useRouter();
  const asked = (useSearchParams().get('speed') ?? '').toLowerCase();
  const speed = SPEEDS.find((k) => SPEED_NAME[k].toLowerCase() === asked) ?? CC_BLITZ;
  return (
    <section className="panel top-boards">
      <div className="seg" role="tablist" aria-label="speed">
        {SPEEDS.map((k) => (
          <button key={k} role="tab" aria-selected={k === speed} className={k === speed ? 'on' : ''}
            onClick={() => router.replace(`/top?speed=${SPEED_NAME[k].toLowerCase()}`)}>{SPEED_NAME[k]}</button>
        ))}
      </div>
      <Leaders speed={speed} limit={50} />
    </section>
  );
}

export default function Top() { return <Suspense><Boards /></Suspense>; }
