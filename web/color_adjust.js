"use strict";

function makeToneCurve(points) {
  if (!points || points.every((value, index) => value === [0, 64, 128, 192, 255][index])) return null;
  const positions = [0, 64, 128, 192, 255];
  const slopes = positions.slice(0, -1).map((position, index) =>
    (points[index + 1] - points[index]) / (positions[index + 1] - position));
  const tangents = [slopes[0]];
  for (let index = 1; index < 4; index++) {
    tangents.push(slopes[index - 1] * slopes[index] === 0 ? 0 :
      2 / (1 / slopes[index - 1] + 1 / slopes[index]));
  }
  tangents.push(slopes[3]);
  const lookup = new Uint8Array(256);
  for (let index = 0, segment = 0; index < 256; index++) {
    while (segment < 3 && index > positions[segment + 1]) segment++;
    const width = positions[segment + 1] - positions[segment];
    const t = (index - positions[segment]) / width;
    const t2 = t * t, t3 = t2 * t;
    const value = (2 * t3 - 3 * t2 + 1) * points[segment] +
      (t3 - 2 * t2 + t) * width * tangents[segment] +
      (-2 * t3 + 3 * t2) * points[segment + 1] +
      (t3 - t2) * width * tangents[segment + 1];
    lookup[index] = Math.round(Math.max(0, Math.min(255, value)));
  }
  return lookup;
}

function adjustColorPixels(source, exposure, saturation, contrast, toneCurve = null) {
  const neutral = exposure === 0 && saturation === 1 && contrast === 1 && !toneCurve;
  const rgba = neutral ? source : new Uint8ClampedArray(source);
  const rgb = new Uint8ClampedArray(source.length / 4 * 3);
  const gain = 2 ** exposure;

  for (let offset = 0, packed = 0; offset < source.length; offset += 4, packed += 3) {
    if (!neutral) {
      const red = source[offset] * gain;
      const green = source[offset + 1] * gain;
      const blue = source[offset + 2] * gain;
      const luma = 0.2126 * red + 0.7152 * green + 0.0722 * blue;
      rgba[offset] = Math.round((luma + (red - luma) * saturation - 128) * contrast + 128);
      rgba[offset + 1] = Math.round((luma + (green - luma) * saturation - 128) * contrast + 128);
      rgba[offset + 2] = Math.round((luma + (blue - luma) * saturation - 128) * contrast + 128);
      if (toneCurve) {
        const brightness = Math.round(0.2126 * rgba[offset] + 0.7152 * rgba[offset + 1] + 0.0722 * rgba[offset + 2]);
        if (brightness > 0) {
          const gain = toneCurve[brightness] / brightness;
          rgba[offset] = Math.round(rgba[offset] * gain);
          rgba[offset + 1] = Math.round(rgba[offset + 1] * gain);
          rgba[offset + 2] = Math.round(rgba[offset + 2] * gain);
        }
      }
    }
    rgb[packed] = rgba[offset];
    rgb[packed + 1] = rgba[offset + 1];
    rgb[packed + 2] = rgba[offset + 2];
  }

  return { rgba, rgb };
}