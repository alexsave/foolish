import { useEffect } from 'react';
import { useWoolTexture } from './WoolBackground';

// Simple reusable component that adds background texture to a page
// Renders both backgrounds - CSS hides the wrong one based on [data-theme]
export const WoolBackgroundLayer = () => {
  const textureUrl = useWoolTexture();

  // Also paint the texture onto the document canvas (html background). The
  // fixed layers below can't cover the iOS home-indicator strip in standalone
  // (PWA) mode, but the html background does — so this keeps the wool seamless
  // all the way to the physical screen edge instead of a flat color band.
  useEffect(() => {
    if (textureUrl) {
      document.documentElement.style.setProperty('--wool-texture', `url(${textureUrl})`);
    }
  }, [textureUrl]);

  return (
    <>
      {/* Default theme: wool texture - CSS hides in Soviet mode */}
      <div
        className="bg-wool"
        style={{
          backgroundImage: textureUrl ? `url(${textureUrl})` : undefined,
        }}
      />
      <div className="bg-vignette bg-vignette--default" />

      {/* Soviet theme: solid background - CSS hides in default mode */}
      <div className="bg-soviet" />
      <div className="bg-vignette bg-vignette--soviet" />
    </>
  );
};
