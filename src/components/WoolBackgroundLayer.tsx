import { useWoolTexture } from './WoolBackground';

// Simple reusable component that adds background texture to a page
// Renders both backgrounds - CSS hides the wrong one based on [data-theme]
export const WoolBackgroundLayer = () => {
  const textureUrl = useWoolTexture();

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
