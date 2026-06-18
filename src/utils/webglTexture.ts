// Shared GPU texture pipeline used by WoodTexture and WoolBackground. Both
// generate per-pixel colours on the CPU, then upload one GL point per pixel and
// read the canvas back as a PNG blob URL. Only the pixel generator and the
// background clear colour differ between the two textures.

export interface PixelData {
    positions: Float32Array;
    colors: Float32Array;
    pointCount: number;
}

// Flatten a width*height*3 colour buffer into parallel GL position/colour
// arrays — one point per pixel, centred on the texel.
export function pixelsToPointArrays(
    pixelColors: Float32Array,
    width: number,
    height: number
): PixelData {
    const totalPixels = width * height;
    const positions = new Float32Array(totalPixels * 2);
    const colors = new Float32Array(totalPixels * 3);

    let pointIndex = 0;
    for (let y = 0; y < height; y++) {
        for (let x = 0; x < width; x++) {
            const idx = (y * width + x) * 3;

            positions[pointIndex * 2] = x + 0.5;
            positions[pointIndex * 2 + 1] = y + 0.5;

            colors[pointIndex * 3] = pixelColors[idx];
            colors[pointIndex * 3 + 1] = pixelColors[idx + 1];
            colors[pointIndex * 3 + 2] = pixelColors[idx + 2];

            pointIndex++;
        }
    }

    return { positions, colors, pointCount: totalPixels };
}

const VERTEX_SHADER = `
      attribute vec2 position;
      attribute vec3 color;
      varying vec3 vColor;
      uniform vec2 resolution;

      void main() {
        vec2 normalized = position / resolution;
        vec2 clipSpace = normalized * 2.0 - 1.0;
        clipSpace.y = -clipSpace.y;
        gl_Position = vec4(clipSpace, 0.0, 1.0);
        gl_PointSize = 1.0;
        vColor = color;
      }
    `;

const FRAGMENT_SHADER = `
      precision mediump float;
      varying vec3 vColor;

      void main() {
        gl_FragColor = vec4(vColor, 1.0);
      }
    `;

// CPU-generate pixel data (via generatePixelData), then draw it on the GPU as a
// point cloud and return a PNG blob URL. Falls back to the supplied CPU path if
// WebGL is unavailable or anything throws — including Safari's toBlob returning
// null at its canvas memory limit (common on iOS at 4K), which we treat as a
// failure so the cheaper CPU path retries instead of caching ''.
export async function generateTextureViaWebGL(
    width: number,
    height: number,
    label: string,
    clearColor: [number, number, number, number],
    generatePixelData: (w: number, h: number) => Promise<PixelData>,
    fallback: (w: number, h: number) => Promise<string>
): Promise<string> {
    const canvas = document.createElement('canvas');
    canvas.width = width;
    canvas.height = height;
    const gl = canvas.getContext('webgl', { preserveDrawingBuffer: true });

    if (!gl) {
        console.error(`WebGL not available for ${label} texture, falling back to CPU-only`);
        return fallback(width, height);
    }

    try {
        // Step 1: Generate pixel data using CPU (like fernFractal generates points)
        const pixelData = await generatePixelData(width, height);

        // Step 2: Use WebGL to draw the pixels efficiently
        const vertexShader = gl.createShader(gl.VERTEX_SHADER)!;
        gl.shaderSource(vertexShader, VERTEX_SHADER);
        gl.compileShader(vertexShader);

        const fragmentShader = gl.createShader(gl.FRAGMENT_SHADER)!;
        gl.shaderSource(fragmentShader, FRAGMENT_SHADER);
        gl.compileShader(fragmentShader);

        const program = gl.createProgram()!;
        gl.attachShader(program, vertexShader);
        gl.attachShader(program, fragmentShader);
        gl.linkProgram(program);
        gl.useProgram(program);

        // Upload position data (one point per pixel)
        const positionBuffer = gl.createBuffer();
        gl.bindBuffer(gl.ARRAY_BUFFER, positionBuffer);
        gl.bufferData(gl.ARRAY_BUFFER, pixelData.positions, gl.STATIC_DRAW);

        const positionLocation = gl.getAttribLocation(program, 'position');
        gl.enableVertexAttribArray(positionLocation);
        gl.vertexAttribPointer(positionLocation, 2, gl.FLOAT, false, 0, 0);

        // Upload color data
        const colorBuffer = gl.createBuffer();
        gl.bindBuffer(gl.ARRAY_BUFFER, colorBuffer);
        gl.bufferData(gl.ARRAY_BUFFER, pixelData.colors, gl.STATIC_DRAW);

        const colorLocation = gl.getAttribLocation(program, 'color');
        gl.enableVertexAttribArray(colorLocation);
        gl.vertexAttribPointer(colorLocation, 3, gl.FLOAT, false, 0, 0);

        // Set uniforms
        const resolutionLocation = gl.getUniformLocation(program, 'resolution');
        gl.uniform2f(resolutionLocation, width, height);

        // Clear and draw all pixels
        gl.clearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
        gl.clear(gl.COLOR_BUFFER_BIT);
        gl.drawArrays(gl.POINTS, 0, pixelData.pointCount);

        gl.flush();
        await new Promise(resolve => setTimeout(resolve, 10));

        return await new Promise<string>((resolve, reject) => {
            canvas.toBlob((blob) => {
                if (blob) {
                    resolve(URL.createObjectURL(blob));
                } else {
                    reject(new Error('canvas.toBlob returned null (canvas memory limit?)'));
                }
            }, 'image/png', 1.0);
        });
    } catch (error) {
        console.error(`WebGL ${label} texture generation failed, using fallback:`, error);
        return fallback(width, height);
    }
}
