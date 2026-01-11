import React, { createContext, useContext, useMemo } from 'react';
import { useTheme } from '../contexts/ThemeContext';
import { useWoodTexture } from './WoodTexture';
import { useConcreteTexture } from './ConcreteTexture';

// Context to share texture URLs - hooks called only once at provider level
interface TextureContextType {
  woodUrl: string | null;
  concreteUrl: string | null;
}

const TextureContext = createContext<TextureContextType | null>(null);

/**
 * Provider that calls texture hooks once and shares URLs via context.
 * Place this high in your component tree (e.g., in App.js).
 */
export const TextureProvider: React.FC<{ children: React.ReactNode }> = ({ children }) => {
  const woodUrl = useWoodTexture();
  const concreteUrl = useConcreteTexture();

  const value = useMemo(() => ({
    woodUrl,
    concreteUrl,
  }), [woodUrl, concreteUrl]);

  return (
    <TextureContext.Provider value={value}>
      {children}
    </TextureContext.Provider>
  );
};

/**
 * Hook to access texture context.
 * Use this when you need to access the texture URLs directly.
 */
export const useTexture = () => {
  const context = useContext(TextureContext);
  if (!context) {
    throw new Error('useTexture must be used within a TextureProvider');
  }
  return context;
};

/**
 * Calculate style from seed and texture URL.
 * Use this when you need to apply texture styles inside a map or callback.
 */
export const getTextureStyle = (
  textureUrl: string | null,
  isSoviet: boolean,
  seed: number,
  willRotate: boolean = false
): React.CSSProperties => {
  const baseWidth = isSoviet ? 512 : 1920;
  const baseHeight = isSoviet ? 512 : 1080;
  const baseColor = isSoviet ? '#4A4A4A' : '#c33f08';
  const fallbackGradient = isSoviet
    ? `repeating-linear-gradient(0deg, transparent, transparent 2px, rgba(0,0,0,0.05) 2px, rgba(0,0,0,0.05) 4px)`
    : `repeating-linear-gradient(90deg, transparent, transparent 1px, rgba(0,0,0,0.1) 1px, rgba(0,0,0,0.1) 2px)`;

  const xOffset = Math.floor(seed * baseWidth);
  const yOffset = Math.floor((seed * 1000) % baseHeight);
  const scaleFactor = willRotate ? 1.5 : 1;
  const scaledWidth = Math.floor(baseWidth * scaleFactor);
  const scaledHeight = Math.floor(baseHeight * scaleFactor);
  const adjustedXOffset = Math.floor(xOffset * scaleFactor);
  const adjustedYOffset = Math.floor(yOffset * scaleFactor);

  return {
    backgroundColor: baseColor,
    backgroundImage: textureUrl ? `url(${textureUrl})` : fallbackGradient,
    backgroundSize: `${scaledWidth}px ${scaledHeight}px`,
    backgroundPosition: `${adjustedXOffset}px ${adjustedYOffset}px`,
    backgroundRepeat: 'repeat',
  };
};

interface TexturedSurfaceProps {
  /** Seed for deterministic texture positioning (0-1 range) */
  seed?: number;
  /** If true, scales texture to prevent gaps during rotation */
  willRotate?: boolean;
  /** Additional CSS classes */
  className?: string;
  /** Additional inline styles */
  style?: React.CSSProperties;
  /** Content to render */
  children?: React.ReactNode;
  /** Element type to render - can be HTML element string or React component */
  as?: React.ElementType;
  /** Pass through any other props to the underlying element */
  [key: string]: any;
}

/**
 * A surface with wood/concrete texture background.
 * Automatically switches based on theme (Russian = concrete, others = wood).
 * 
 * @example
 * <TexturedSurface seed={0.5} className="btn-wood">
 *   <span>Click me</span>
 * </TexturedSurface>
 * 
 * @example
 * // Derive seed from player ID
 * <TexturedSurface seed={(playerId.charCodeAt(0) + playerId.charCodeAt(1)) / 200}>
 *   {content}
 * </TexturedSurface>
 */
export const TexturedSurface: React.FC<TexturedSurfaceProps> = ({
  seed = 0.5,
  willRotate = false,
  className = '',
  style = {},
  children,
  as: Component = 'div' as React.ElementType,
  ...props
}) => {
  const { isSoviet } = useTheme();
  const { woodUrl, concreteUrl } = useTexture();
  const textureUrl = isSoviet ? concreteUrl : woodUrl;

  const textureStyle = useMemo(
    () => getTextureStyle(textureUrl, isSoviet, seed, willRotate),
    [textureUrl, isSoviet, seed, willRotate]
  );

  return (
    <Component
      className={className}
      style={{ ...textureStyle, ...style }}
      {...props}
    >
      {children}
    </Component>
  );
};

/**
 * Helper to generate a consistent seed from a string (like player ID)
 */
export const seedFromString = (str: string): number => {
  if (!str || str.length < 2) return 0.5;
  return (str.charCodeAt(0) + str.charCodeAt(1)) / 200;
};

/**
 * Helper to determine flip direction from a string
 */
export const flipFromString = (str: string): number => {
  if (!str || str.length < 4) return 1;
  return (str.charCodeAt(3) || 0) % 2 === 0 ? 1 : -1;
};
