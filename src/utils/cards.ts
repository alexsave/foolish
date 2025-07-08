
  export const generateCardBackPattern = (width: number, height: number): string => {
    const canvas = document.createElement('canvas');
    canvas.width = width;
    canvas.height = height;
    const ctx = canvas.getContext('2d')!;

    const imageData = ctx.createImageData(width, height);
    const data = imageData.data;

    // Red background color (#DC143C crimson red)
    const bgRed = 220;
    const bgGreen = 20;
    const bgBlue = 60;

    // Gold line color (#FFD700 gold)
    const lineRed = 255;
    const lineGreen = 215;
    const lineBlue = 0;

    // Scale grid size proportionally to card size (base size is 40x70)
    const baseWidth = 40;
    const scaleFactor = width / baseWidth;
    const gridSize = 8 * scaleFactor; // Scale the grid size
    const lineWidth = 0.5 * scaleFactor; // Scale line width too

    // Calculate angles for 30° and -30° diagonals
    const angle1 = Math.PI / 3; // 30 degrees
    const angle2 = -Math.PI / 3; // -30 degrees

    // Direction vectors for the diagonal lines
    const cos1 = Math.cos(angle1);
    const sin1 = Math.sin(angle1);
    const cos2 = Math.cos(angle2);
    const sin2 = Math.sin(angle2);

    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const index = (y * width + x) * 4;

        // Calculate distance to diagonal lines using line equation
        // For 30° diagonal: distance from point to parallel lines spaced gridSize apart
        const dist1 = Math.abs((x * sin1 - y * cos1) % gridSize);
        const dist2 = Math.abs((x * sin2 - y * cos2) % gridSize);

        // Check if point is close enough to either diagonal line
        const onLine1 = Math.min(dist1, gridSize - dist1) < lineWidth;
        const onLine2 = Math.min(dist2, gridSize - dist2) < lineWidth;

        if (onLine1 || onLine2) {
          // Gold line
          data[index] = lineRed;
          data[index + 1] = lineGreen;
          data[index + 2] = lineBlue;
        } else {
          // Red background
          data[index] = bgRed;
          data[index + 1] = bgGreen;
          data[index + 2] = bgBlue;
        }
        data[index + 3] = 255; // Alpha
      }
    }

    ctx.putImageData(imageData, 0, 0);
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