// flightTiming.ts - HOW LATE THE PAGE PAINTS WHAT THE PLAN SAYS, in one place.
//
// The kernel's plan says when each step starts and lands. The page shows it a beat after:
// a flight stands still for FLIGHT_ARM_MS (one crossed paint, so its start frame renders
// where the card is) before it moves. A SOUND struck for the plan's moment would be heard
// that much BEFORE the card it is the sound of, so the sounds (and what is drawn to go with
// them) wait the same beat. That is the whole of it: a number in tuning.tsv is where in
// the sound a part sits and never "plus however late the screen is", so nobody tunes a
// card landing "+40 ms" to hear it when it lands.

/** How long a flight stands still before its transition is armed: one crossed
 *  paint, so the start frame renders where the card actually is (see `progress`
 *  in FlightCard - AnimationOverlay arms it on this timer). Anything that has to
 *  MOVE WITH a flight has to wait the same beat, or it leads the card it is
 *  supposed to be moving with: the battle grid's counter-tilt does (TableBattles),
 *  and so do the sounds (PAINT_LAG_MS). It is a paint, not choreography - the
 *  kernel owns the duration, and this is the browser's own latency before it begins. */
export const FLIGHT_ARM_MS = 25;

/** How long after the plan's moment the page SHOWS it: the beat a sound struck for that
 *  moment waits so it is heard as the card moves or lands (useAnimationRun). */
export const PAINT_LAG_MS = FLIGHT_ARM_MS;
