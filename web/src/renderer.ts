/**
 * Draws a colour and alpha picture pair into a canvas, premultiplied: with the WebGL2 context the
 * players share, then copied into this player's 2D canvas. YUV planes go through the native
 * players' matrix (cpp/YuvToRgb), so colours match iOS and Android; pictures only available as
 * frames (Firefox) are converted by the browser.
 */
import type { FramePair, Picture, Planes } from './source';
import { SharedGL } from './shared-gl';

export interface DrawParams {
  /** YUV to RGB for colour planes: a column-major mat4 from yuvToRgbMatrix */
  matrix: Float32Array;
  /** Alpha planes hold 0–255, not 16–235 */
  alphaFullRange: boolean;
  /**
   * Alpha drawn by the browser from a full-range stream it took for limited range (VP9
   * BlockAdditional alpha), so the expansion it applied is undone
   */
  alphaUndoExpansion: boolean;
}

// EXT_texture_norm16 formats
const R16_EXT = 0x822a;
const RG16_EXT = 0x822c;

interface Texture {
  texture: WebGLTexture;
  key: string;
}

export class AlphaRenderer {
  readonly canvas: HTMLCanvasElement;
  private readonly context: CanvasRenderingContext2D;
  private readonly shared: SharedGL;
  private textures: Texture[];
  /** The shared context's generation the textures were made in */
  private generation: number;

  constructor(canvas: HTMLCanvasElement) {
    this.shared = SharedGL.acquire();
    this.canvas = canvas;
    this.context = canvas.getContext('2d')!;
    this.textures = createTextures(this.shared.gl);
    this.generation = this.shared.generation;
  }

  draw(pair: FramePair, params: DrawParams): void {
    const shared = this.shared;
    if (shared.lost) return;
    if (this.generation !== shared.generation) {
      this.textures = createTextures(shared.gl);
      this.generation = shared.generation;
    }
    const gl = shared.gl;
    const { color, alpha } = pair;
    const width = pictureWidth(color);
    const height = pictureHeight(color);
    shared.fit(width, height);
    gl.viewport(0, 0, width, height);
    gl.useProgram(shared.program);

    let colorMode = 2;
    let colorScale = 1;
    if (color.kind === 'planes') {
      colorMode = color.interleavedChroma ? 1 : 0;
      colorScale = this.uploadPlane(0, color, 0, color.width, color.height, 1);
      if (color.interleavedChroma) {
        this.uploadPlane(1, color, 1, color.chromaWidth, color.chromaHeight, 2);
      } else {
        this.uploadPlane(1, color, 1, color.chromaWidth, color.chromaHeight, 1);
        this.uploadPlane(2, color, 2, color.chromaWidth, color.chromaHeight, 1);
      }
    } else {
      this.uploadFrame(0, color.frame);
    }

    let alphaMode: number;
    let alphaScale = 1;
    if (alpha.kind === 'planes') {
      alphaMode = params.alphaFullRange ? 1 : 0;
      alphaScale = this.uploadPlane(3, alpha, 0, alpha.width, alpha.height, 1);
    } else {
      alphaMode = params.alphaUndoExpansion ? 3 : 2;
      this.uploadFrame(3, alpha.frame);
    }

    const u = shared.uniforms;
    gl.uniformMatrix4fv(u.uMatrix, false, params.matrix);
    gl.uniform1i(u.uColorMode, colorMode);
    gl.uniform1i(u.uAlphaMode, alphaMode);
    gl.uniform1f(u.uColorScale, colorScale);
    gl.uniform1f(u.uAlphaScale, alphaScale);
    gl.clearColor(0, 0, 0, 0);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    this.copyFromShared(width, height);
  }

  /** Empties the canvas, e.g. after stop(). */
  clear(): void {
    this.context.clearRect(0, 0, this.canvas.width, this.canvas.height);
  }

  dispose(): void {
    const shared = this.shared;
    if (!shared.lost && this.generation === shared.generation) {
      for (const t of this.textures) shared.gl.deleteTexture(t.texture);
    }
    shared.release();
  }

  /**
   * Copies the picture just drawn, before the next player draws over it. WebGL's rows run bottom
   * up, so it's the bottom-left corner of the shared canvas.
   */
  private copyFromShared(width: number, height: number): void {
    const source = this.shared.canvas;
    if (this.canvas.width !== width || this.canvas.height !== height) {
      this.canvas.width = width;
      this.canvas.height = height;
    } else {
      this.context.clearRect(0, 0, width, height);
    }
    this.context.drawImage(source, 0, source.height - height, width, height, 0, 0, width, height);
  }

  /**
   * Uploads plane `plane` of `p` to texture `unit`. Returns the factor that scales its samples
   * to 0–1: 16-bit textures hold 10- or 12-bit values.
   */
  private uploadPlane(unit: number, p: Planes, plane: number, width: number, height: number, channels: 1 | 2): number {
    const gl = this.shared.gl;
    const { offset, stride } = p.layout[plane];
    const bytes = p.bytesPerSample;
    gl.activeTexture(gl.TEXTURE0 + unit);
    const t = this.textures[unit];
    gl.bindTexture(gl.TEXTURE_2D, t.texture);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);

    const format = channels === 2 ? gl.RG : gl.RED;
    let data: ArrayBufferView = p.data.subarray(offset);
    let internal: number = channels === 2 ? gl.RG8 : gl.R8;
    let type: number = gl.UNSIGNED_BYTE;
    let rowLength = stride / (bytes * channels);
    let scale = 1;
    if (bytes === 2) {
      const maxValue = (1 << p.bitDepth) - 1;
      const byteOffset = p.data.byteOffset + offset;
      if (this.shared.norm16 && byteOffset % 2 === 0) {
        data = new Uint16Array(p.data.buffer, byteOffset, (p.data.length - offset) >> 1);
        internal = channels === 2 ? RG16_EXT : R16_EXT;
        type = gl.UNSIGNED_SHORT;
        scale = 65535 / maxValue;
      } else {
        data = toEightBit(p.data, offset, stride, width * channels, height, p.bitDepth);
        rowLength = 0;
      }
    }
    gl.pixelStorei(gl.UNPACK_ROW_LENGTH, rowLength);

    const key = `${internal}:${width}x${height}`;
    if (t.key !== key) {
      gl.texImage2D(gl.TEXTURE_2D, 0, internal, width, height, 0, format, type, data);
      t.key = key;
    } else {
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, width, height, format, type, data);
    }
    gl.pixelStorei(gl.UNPACK_ROW_LENGTH, 0);
    return scale;
  }

  private uploadFrame(unit: number, frame: VideoFrame): void {
    const gl = this.shared.gl;
    gl.activeTexture(gl.TEXTURE0 + unit);
    const t = this.textures[unit];
    gl.bindTexture(gl.TEXTURE_2D, t.texture);
    gl.pixelStorei(gl.UNPACK_ROW_LENGTH, 0);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, frame);
    t.key = '';
  }
}

/** One texture per sampler: Y, U, V and alpha. */
function createTextures(gl: WebGL2RenderingContext): Texture[] {
  return [0, 1, 2, 3].map(() => {
    const texture = gl.createTexture()!;
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    return { texture, key: '' };
  });
}

function pictureWidth(p: Picture): number {
  return p.kind === 'planes' ? p.width : p.frame.displayWidth;
}

function pictureHeight(p: Picture): number {
  return p.kind === 'planes' ? p.height : p.frame.displayHeight;
}

/** 10- or 12-bit little-endian samples to 8-bit, for WebGL without 16-bit normalised textures. */
function toEightBit(src: Uint8Array, offset: number, stride: number, samples: number, rows: number, bitDepth: number): Uint8Array {
  const out = new Uint8Array(samples * rows);
  const shift = bitDepth - 8;
  for (let y = 0; y < rows; y++) {
    let s = offset + y * stride;
    const o = y * samples;
    for (let x = 0; x < samples; x++, s += 2) out[o + x] = Math.min(255, (src[s] | (src[s + 1] << 8)) >> shift);
  }
  return out;
}
