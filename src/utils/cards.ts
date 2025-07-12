// Use the actual Khokhloma pattern PNG
const FRACTAL_PATTERN_PNG = "/khokhloma-pattern.png";

// Main export - optimized version using pre-generated PNG
export const generateCardBackPattern = async (width: number, height: number): Promise<string> => {
    return new Promise((resolve, reject) => {
        const canvas = document.createElement('canvas');
        const ctx = canvas.getContext('2d')!;
        
        // Set canvas size with device pixel ratio for crisp rendering
        const dpr = window.devicePixelRatio || 1;
        canvas.width = width * dpr;
        canvas.height = height * dpr;
        canvas.style.width = width + 'px';
        canvas.style.height = height + 'px';
        ctx.scale(dpr, dpr);
        
        // Enable high-quality image smoothing for better downscaling
        ctx.imageSmoothingEnabled = true;
        ctx.imageSmoothingQuality = 'high';
        
        // Black background
        ctx.fillStyle = '#000000';
        ctx.fillRect(0, 0, width, height);
        
        // Load the pre-generated fractal pattern
        const img = new Image();
        img.onload = () => {
            // Stretch the pattern to fill the entire card
            ctx.drawImage(img, 0, 0, width, height);
            
            resolve(canvas.toDataURL());
        };
        img.onerror = () => {
            console.error('Failed to load fractal pattern, using black background');
            // Just return black background if image fails to load
            resolve(canvas.toDataURL());
        };
        img.src = FRACTAL_PATTERN_PNG;
    });
};

// Optimized version using pre-generated PNG (legacy name)
export const generateCardBackPatternOptimized = generateCardBackPattern;

// Even more optimized version if you have the ImageData directly
export const generateCardBackPatternFromImageData = (width: number, height: number, sourceImageData: ImageData): string => {
    const canvas = document.createElement('canvas');
    const ctx = canvas.getContext('2d')!;
    
    // Set canvas size with device pixel ratio
    const dpr = window.devicePixelRatio || 1;
    canvas.width = width * dpr;
    canvas.height = height * dpr;
    canvas.style.width = width + 'px';
    canvas.style.height = height + 'px';
    ctx.scale(dpr, dpr);
    
    // Enable high-quality image smoothing for better downscaling
    ctx.imageSmoothingEnabled = true;
    ctx.imageSmoothingQuality = 'high';
    
    // Black background
    ctx.fillStyle = '#000000';
    ctx.fillRect(0, 0, width, height);
    
    // Create temporary canvas for the source pattern
    const tempCanvas = document.createElement('canvas');
    const tempCtx = tempCanvas.getContext('2d')!;
    tempCanvas.width = sourceImageData.width;
    tempCanvas.height = sourceImageData.height;
    tempCtx.putImageData(sourceImageData, 0, 0);
    
    // Scale and draw the pattern
    const scale = Math.min(width / sourceImageData.width, height / sourceImageData.height);
    const scaledWidth = sourceImageData.width * scale;
    const scaledHeight = sourceImageData.height * scale;
    const x = (width - scaledWidth) / 2;
    const y = (height - scaledHeight) / 2;
    
    ctx.drawImage(tempCanvas, x, y, scaledWidth, scaledHeight);
    
    return canvas.toDataURL();
};

export const SUIT_MAP: Record<number, string> = {
    // emojis
    0: '♠️',
    1: '♥️',
    2: '♣️',
    3: '♦️',
}

export const VALUE_MAP: Record<number, string> = {
    1: '2',
    2: '3',
    3: '4',
    4: '5',
    5: '6',
    6: '7',
    7: '8',
    8: '9',
    9: '10',
    10: 'J',
    11: 'Q',
    12: 'K',
    13: 'A',
}

// Ok let's actually look at the game state to see if we are defending and modify options