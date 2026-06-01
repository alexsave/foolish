import { WoolBackgroundLayer } from './WoolBackgroundLayer';

// Full-screen, on-theme loading state. Renders the same textured background
// every page uses, plus a spinner that fades in only if loading takes a moment.
// Used for auth/route/data transitions so they never flash a bare empty screen.
export const LoadingScreen = () => {
  return (
    <div className="page page--centered page--full-viewport">
      <WoolBackgroundLayer />
      <div className="loading-spinner" role="status" aria-label="Loading" />
    </div>
  );
};
