import { useWoolTexture } from './WoolBackground';

// Simple reusable component that adds wool texture background to a page
// Uses the hook to trigger async generation that doesn't block React
export const WoolBackgroundLayer = () => {
  const textureUrl = useWoolTexture();
  
  const containerStyle: React.CSSProperties = {
    position: 'fixed',
    top: 0,
    left: 0,
    width: '100vw',
    height: '100vh',
    zIndex: -1,
  };

  const vignetteStyle: React.CSSProperties = {
    ...containerStyle,
    zIndex: 0, // One layer above the wool texture
    background: `radial-gradient(ellipse at center, 
      rgba(101, 67, 33, 0) 0%, 
      rgba(101, 67, 33, 0) 40%, 
      rgba(101, 67, 33, 0.2) 70%, 
      rgba(101, 67, 33, 0.6) 100%)`,
    pointerEvents: 'none'
  };
  
  return (
    <>
      <div style={{ 
        ...containerStyle,
        backgroundColor: '#cac5af', // Fallback color
        backgroundImage: textureUrl ? `url(${textureUrl})` : undefined,
        backgroundSize: 'cover',
        objectFit: 'cover',
        transform: 'scale(2)',
        backgroundPosition: 'center',
        backgroundRepeat: 'no-repeat'
      }} />
      <div style={vignetteStyle} />
    </>
  );
};

