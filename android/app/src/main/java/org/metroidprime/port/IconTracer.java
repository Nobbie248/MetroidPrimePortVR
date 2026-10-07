package org.metroidprime.port;

import android.graphics.Path;

// Turns one of the game's small HUD icons (alpha = coverage) into a vector outline, so the
// wheels can draw it sharp at any size without shipping any art of the game's: the icon
// is upsampled with a bicubic filter, its half-coverage contour is traced with marching
// squares and each loop is simplified with Douglas-Peucker.
final class IconTracer {
    private static final int PAD = 2; // empty border, so contours touching the edge close
    private static final int UP = 8; // field samples per icon pixel
    private static final float THRESHOLD = 0.5f;
    private static final float TOLERANCE = 0.08f; // simplification error, in icon pixels

    private IconTracer() {
    }

    // The outline in icon pixel units (0..w, 0..h), filled even-odd; null when it is empty.
    static Path trace(int[] argb, int offset, int w, int h) {
        final int sw = w + 2 * PAD;
        final int sh = h + 2 * PAD;
        final float[] src = new float[sw * sh];
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                src[(y + PAD) * sw + x + PAD] = (argb[offset + y * w + x] >>> 24) / 255f;
            }
        }
        final int nx = sw * UP + 1;
        final int ny = sh * UP + 1;
        final float[] field = upsample(src, sw, sh, nx, ny);

        // Each crossed grid edge links to the two edges it shares a cell segment with.
        // Edge ids: 2 * (y * nx + x) for (x,y)-(x+1,y), + 1 for (x,y)-(x,y+1).
        final int[] link = new int[nx * ny * 4];
        java.util.Arrays.fill(link, -1);
        for (int y = 0; y + 1 < ny; ++y) {
            for (int x = 0; x + 1 < nx; ++x) {
                final float a = field[y * nx + x];
                final float b = field[y * nx + x + 1];
                final float c = field[(y + 1) * nx + x + 1];
                final float d = field[(y + 1) * nx + x];
                final int code = (a > THRESHOLD ? 1 : 0) | (b > THRESHOLD ? 2 : 0) |
                                 (c > THRESHOLD ? 4 : 0) | (d > THRESHOLD ? 8 : 0);
                if (code == 0 || code == 15) {
                    continue;
                }
                final int top = 2 * (y * nx + x);
                final int bottom = 2 * ((y + 1) * nx + x);
                final int left = top + 1;
                final int right = 2 * (y * nx + x + 1) + 1;
                final boolean centre = (a + b + c + d) / 4f > THRESHOLD;
                switch (code) {
                    case 1: case 14: connect(link, left, top); break;
                    case 2: case 13: connect(link, top, right); break;
                    case 3: case 12: connect(link, left, right); break;
                    case 4: case 11: connect(link, right, bottom); break;
                    case 6: case 9: connect(link, top, bottom); break;
                    case 7: case 8: connect(link, left, bottom); break;
                    case 5:
                        if (centre) {
                            connect(link, left, bottom);
                            connect(link, top, right);
                        } else {
                            connect(link, left, top);
                            connect(link, right, bottom);
                        }
                        break;
                    case 10:
                        if (centre) {
                            connect(link, left, top);
                            connect(link, right, bottom);
                        } else {
                            connect(link, left, bottom);
                            connect(link, top, right);
                        }
                        break;
                    default:
                        break;
                }
            }
        }

        final Path path = new Path();
        path.setFillType(Path.FillType.EVEN_ODD);
        final boolean[] seen = new boolean[nx * ny * 2];
        float[] loop = new float[256];
        boolean any = false;
        for (int start = 0; start < seen.length; ++start) {
            if (seen[start] || link[start * 2] < 0) {
                continue;
            }
            int count = 0;
            int prev = -1;
            int edge = start;
            while (edge >= 0 && !seen[edge]) {
                seen[edge] = true;
                if (count * 2 + 2 > loop.length) {
                    loop = java.util.Arrays.copyOf(loop, loop.length * 2);
                }
                crossing(field, nx, edge, loop, count++);
                final int l0 = link[edge * 2];
                final int next = l0 != prev ? l0 : link[edge * 2 + 1];
                prev = edge;
                edge = next;
            }
            if (count < 3) {
                continue;
            }
            addSimplified(path, loop, count);
            any = true;
        }
        return any ? path : null;
    }

    private static void connect(int[] link, int e0, int e1) {
        link[e0 * 2 + (link[e0 * 2] < 0 ? 0 : 1)] = e1;
        link[e1 * 2 + (link[e1 * 2] < 0 ? 0 : 1)] = e0;
    }

    // Where the contour crosses an edge, in icon pixels, written to out[i].
    private static void crossing(float[] field, int nx, int edge, float[] out, int i) {
        final int cell = edge >> 1;
        final int x = cell % nx;
        final int y = cell / nx;
        final boolean vertical = (edge & 1) != 0;
        final float f0 = field[cell];
        final float f1 = field[vertical ? cell + nx : cell + 1];
        final float t = (f0 - THRESHOLD) / (f0 - f1);
        final float px = vertical ? x : x + t;
        final float py = vertical ? y + t : y;
        out[i * 2] = px / UP - PAD;
        out[i * 2 + 1] = py / UP - PAD;
    }

    // Catmull-Rom upsampling; field sample (i, j) sits at icon coordinate (i, j) / UP,
    // with pixel k's centre at k + 0.5.
    private static float[] upsample(float[] src, int sw, int sh, int nx, int ny) {
        final float[] rows = new float[sh * nx];
        for (int s = 0; s < nx; ++s) {
            final float pos = (float) s / UP - 0.5f;
            final int k0 = (int) Math.floor(pos) - 1;
            for (int k = k0; k < k0 + 4; ++k) {
                if (k < 0 || k >= sw) {
                    continue;
                }
                final float wgt = cubic(pos - k);
                for (int r = 0; r < sh; ++r) {
                    rows[r * nx + s] += wgt * src[r * sw + k];
                }
            }
        }
        final float[] field = new float[ny * nx];
        for (int t = 0; t < ny; ++t) {
            final float pos = (float) t / UP - 0.5f;
            final int k0 = (int) Math.floor(pos) - 1;
            for (int k = k0; k < k0 + 4; ++k) {
                if (k < 0 || k >= sh) {
                    continue;
                }
                final float wgt = cubic(pos - k);
                for (int s = 0; s < nx; ++s) {
                    field[t * nx + s] += wgt * rows[k * nx + s];
                }
            }
        }
        return field;
    }

    private static float cubic(float t) {
        t = Math.abs(t);
        if (t < 1f) {
            return (1.5f * t - 2.5f) * t * t + 1f;
        }
        if (t < 2f) {
            return ((-0.5f * t + 2.5f) * t - 4f) * t + 2f;
        }
        return 0f;
    }

    // Adds the closed loop as a polygon, keeping only the points that matter: it is split
    // at the point farthest from the first, and each half is simplified on its own.
    private static void addSimplified(Path path, float[] pts, int count) {
        int far = 0;
        float best = -1f;
        for (int i = 1; i < count; ++i) {
            final float dx = pts[i * 2] - pts[0];
            final float dy = pts[i * 2 + 1] - pts[1];
            if (dx * dx + dy * dy > best) {
                best = dx * dx + dy * dy;
                far = i;
            }
        }
        final boolean[] keep = new boolean[count];
        keep[0] = true;
        keep[far] = true;
        mark(pts, count, 0, far, keep);
        mark(pts, count, far, count, keep);
        boolean first = true;
        for (int i = 0; i < count; ++i) {
            if (!keep[i]) {
                continue;
            }
            if (first) {
                path.moveTo(pts[i * 2], pts[i * 2 + 1]);
                first = false;
            } else {
                path.lineTo(pts[i * 2], pts[i * 2 + 1]);
            }
        }
        path.close();
    }

    // Douglas-Peucker over points a..b (b may equal count, meaning point 0 again).
    private static void mark(float[] pts, int count, int a, int b, boolean[] keep) {
        if (b - a < 2) {
            return;
        }
        final int bi = b % count;
        final float x0 = pts[a * 2];
        final float y0 = pts[a * 2 + 1];
        final float dx = pts[bi * 2] - x0;
        final float dy = pts[bi * 2 + 1] - y0;
        final float len = Math.max((float) Math.hypot(dx, dy), 1e-6f);
        int worst = -1;
        float dist = TOLERANCE;
        for (int i = a + 1; i < b; ++i) {
            final float d = Math.abs(dy * (pts[i * 2] - x0) - dx * (pts[i * 2 + 1] - y0)) / len;
            if (d > dist) {
                dist = d;
                worst = i;
            }
        }
        if (worst < 0) {
            return;
        }
        keep[worst] = true;
        mark(pts, count, a, worst, keep);
        mark(pts, count, worst, b, keep);
    }
}
