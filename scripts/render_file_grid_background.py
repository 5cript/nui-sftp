#!/usr/bin/env python3
# Renders the file grid background images into nui-file-explorer/styles.
# Seven radial gradients multiplied over the grid background color, following CSS Images 3 (farthest-corner
# ellipses, premultiplied interpolation) and Compositing 1 (separable blend, then source-over) in sRGB.
# The image holds only what the gradients add, drawn over that same background color it reproduces the blend.
# WebKitGTK paints the live gradients on the CPU on every repaint, a stretched image is far cheaper.
# Usage: render_file_grid_background.py [width] [height]
import colorsys
import sys
from pathlib import Path

import numpy as np
from PIL import Image

styles = Path(__file__).resolve().parent.parent / "nui-file-explorer/styles"
width = int(sys.argv[1]) if len(sys.argv) > 1 else 160
height = int(sys.argv[2]) if len(sys.argv) > 2 else 128

# (x %, y %, hue, saturation %, lightness %), first entry is the top layer
spots = [
    (56, 8, 323, 64, 77),
    (88, 7, 186, 94, 76),
    (67, 1, 321, 80, 79),
    (60, 69, 39, 75, 79),
    (22, 33, 314, 83, 65),
    (7, 91, 200, 78, 70),
    (19, 86, 137, 62, 74),
]

# file name -> background color (r, g, b, a) of .nui-file-grid-background in that theme
variants = {
    "file_grid_background.png": (0, 0, 0, 0.55),
    "file_grid_background_light.png": (224 / 255, 224 / 255, 224 / 255, 1),
}


def blendedGradients(backdropColor, backdropAlpha):
    rows, columns = np.mgrid[0:height, 0:width].astype(np.float64) + 0.5
    premultiplied = np.broadcast_to(np.array(backdropColor) * backdropAlpha, (height, width, 3)).copy()
    alpha = np.full((height, width), backdropAlpha)
    for spotX, spotY, hue, saturation, lightness in reversed(spots):
        source = np.array(colorsys.hls_to_rgb(hue / 360, lightness / 100, saturation / 100))
        centerX = spotX / 100 * width
        centerY = spotY / 100 * height
        radiusX = max(centerX, width - centerX) * np.sqrt(2)
        radiusY = max(centerY, height - centerY) * np.sqrt(2)
        distance = np.sqrt(((columns - centerX) / radiusX) ** 2 + ((rows - centerY) / radiusY) ** 2)
        sourceAlpha = np.clip(1 - distance / 0.5, 0, 1)[..., None]

        layerAlpha = alpha[..., None]
        backdrop = premultiplied / np.maximum(layerAlpha, 1e-9)
        blended = (1 - layerAlpha) * source + layerAlpha * source * backdrop
        premultiplied = sourceAlpha * blended + (1 - sourceAlpha) * premultiplied
        alpha = sourceAlpha[..., 0] + alpha * (1 - sourceAlpha[..., 0])
    return premultiplied, alpha


for fileName, (red, green, blue, backdropAlpha) in variants.items():
    backdropColor = np.array([red, green, blue])
    premultiplied, alpha = blendedGradients(backdropColor, backdropAlpha)
    if backdropAlpha >= 1:
        imageAlpha = np.ones_like(alpha)
        imagePremultiplied = premultiplied
    else:
        # Solve image over backdrop == blended result
        imageAlpha = np.clip((alpha - backdropAlpha) / (1 - backdropAlpha), 0, 1)
        imagePremultiplied = premultiplied - (1 - imageAlpha[..., None]) * backdropAlpha * backdropColor
    color = np.clip(imagePremultiplied / np.maximum(imageAlpha[..., None], 1e-9), 0, 1)
    pixels = np.round(np.dstack([color, imageAlpha]) * 255).astype(np.uint8)
    Image.fromarray(pixels, "RGBA").save(styles / fileName, optimize=True)
    print(f"{styles / fileName} {width}x{height}")
