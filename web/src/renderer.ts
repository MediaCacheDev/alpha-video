/**
 * Draws a colour and alpha picture pair into a canvas with WebGL2, premultiplied. YUV planes go
 * through the native players' matrix (cpp/YuvToRgb), so colours match iOS and Android; pictures
 * only available as frames (Firefox) are converted by the browser.
 */
import { AlphaVideoError } from './errors';
import type { FramePair, Picture, Planes } from './source';

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

const VERTEX = `#version 300 es
out vec2 vUv;
void main() {
  vec2 p = vec2(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0);
  vUv = vec2(p.x + 1.0, 1.0 - p.y) * 0.5;
  gl_Position = vec4(p, 0.0, 1.0);
}`;

const FRAGMENT = `#version 300 es
precision highp float;
uniform sampler2D uY, uU, uV, uA;
uniform mat4 uMatrix;
uniform int uColorMode;  // 0 planar YUV, 1 NV12, 2 RGB
uniform int uAlphaMode;  // 0 limited-range plane, 1 full-range plane, 2 RGB, 3 RGB to undo
uniform float uColorScale, uAlphaScale;
in vec2 vUv;
out vec4 outColor;
void main() {
  vec3 rgb;
  if (uColorMode == 2) {
    rgb = texture(uY, vUv).rgb;
  } else {
    float y = texture(uY, vUv).r * uColorScale;
    vec2 uv = uColorMode == 1 ? texture(uU, vUv).rg : vec2(texture(uU, vUv).r, texture(uV, vUv).r);
    rgb = clamp((uMatrix * vec4(y, uv * uColorScale, 1.0)).rgb, 0.0, 1.0);
  }
  float a = texture(uA, vUv).r * uAlphaScale;
  if (uAlphaMode == 0) a = (a - 16.0 / 255.0) * (255.0 / 219.0);
  // The browser clipped what it expanded: 0-16 all read 0, 235-255 all read 1
  else if (uAlphaMode == 3) a = a <= 0.0 || a >= 1.0 ? a : a * (219.0 / 255.0) + 16.0 / 255.0;
  a = clamp(a, 0.0, 1.0);
  outColor = vec4(rgb * a, a);
}`;

// EXT_texture_norm16 formats
const R16_EXT = 0x822a;
const RG16_EXT = 0x822c;

interface Texture {
  texture: WebGLTexture;
  key: string;
}

export class AlphaRenderer {
  readonly canvas: HTMLCanvasElement | OffscreenCanvas;
  private gl: WebGL2RenderingContext;
  private program!: WebGLProgram;
  private uniforms!: Record<string, WebGLUniformLocation | null>;
  private textures!: Texture[];
  private norm16 = false;
  private lost = false;

  constructor(canvas: HTMLCanvasElement | OffscreenCanvas) {
    this.canvas = canvas;
    const gl = canvas.getContext('webgl2', {
      alpha: true,
      premultipliedAlpha: true,
      antialias: false,
      depth: false,
      stencil: false,
      preserveDrawingBuffer: false,
    }) as WebGL2RenderingContext | null;
    if (!gl) throw new AlphaVideoError('WEBGL_UNSUPPORTED', 'WebGL2 is unavailable');
    this.gl = gl;
    this.setUp();
    canvas.addEventListener('webglcontextlost', this.onLost as EventListener);
    canvas.addEventListener('webglcontextrestored', this.onRestored as EventListener);
  }

  draw(pair: FramePair, params: DrawParams): void {
    if (this.lost) return;
    const gl = this.gl;
    const { color, alpha } = pair;
    const width = pictureWidth(color);
    const height = pictureHeight(color);
    if (this.canvas.width !== width || this.canvas.height !== height) {
      this.canvas.width = width;
      this.canvas.height = height;
    }
    gl.viewport(0, 0, width, height);
    gl.useProgram(this.program);

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

    const u = this.uniforms;
    gl.uniformMatrix4fv(u.uMatrix, false, params.matrix);
    gl.uniform1i(u.uColorMode, colorMode);
    gl.uniform1i(u.uAlphaMode, alphaMode);
    gl.uniform1f(u.uColorScale, colorScale);
    gl.uniform1f(u.uAlphaScale, alphaScale);
    gl.clearColor(0, 0, 0, 0);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
  }

  /** Empties the canvas, e.g. after stop(). */
  clear(): void {
    if (this.lost) return;
    this.gl.clearColor(0, 0, 0, 0);
    this.gl.clear(this.gl.COLOR_BUFFER_BIT);
  }

  dispose(): void {
    this.canvas.removeEventListener('webglcontextlost', this.onLost as EventListener);
    this.canvas.removeEventListener('webglcontextrestored', this.onRestored as EventListener);
    if (this.lost) return;
    for (const t of this.textures) this.gl.deleteTexture(t.texture);
    this.gl.deleteProgram(this.program);
    this.gl.getExtension('WEBGL_lose_context')?.loseContext();
  }

  private onLost = (e: Event) => {
    e.preventDefault();
    this.lost = true;
  };

  private onRestored = () => {
    this.lost = false;
    this.setUp();
  };

  private setUp(): void {
    const gl = this.gl;
    this.norm16 = !!gl.getExtension('EXT_texture_norm16');
    this.program = linkProgram(gl, VERTEX, FRAGMENT);
    const names = ['uY', 'uU', 'uV', 'uA', 'uMatrix', 'uColorMode', 'uAlphaMode', 'uColorScale', 'uAlphaScale'];
    this.uniforms = Object.fromEntries(names.map((n) => [n, gl.getUniformLocation(this.program, n)]));
    gl.useProgram(this.program);
    ['uY', 'uU', 'uV', 'uA'].forEach((n, unit) => gl.uniform1i(this.uniforms[n], unit));
    this.textures = [0, 1, 2, 3].map(() => {
      const texture = gl.createTexture()!;
      gl.bindTexture(gl.TEXTURE_2D, texture);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
      return { texture, key: '' };
    });
  }

  /**
   * Uploads plane `plane` of `p` to texture `unit`. Returns the factor that scales its samples
   * to 0–1: 16-bit textures hold 10- or 12-bit values.
   */
  private uploadPlane(unit: number, p: Planes, plane: number, width: number, height: number, channels: 1 | 2): number {
    const gl = this.gl;
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
      if (this.norm16 && byteOffset % 2 === 0) {
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
    const gl = this.gl;
    gl.activeTexture(gl.TEXTURE0 + unit);
    const t = this.textures[unit];
    gl.bindTexture(gl.TEXTURE_2D, t.texture);
    gl.pixelStorei(gl.UNPACK_ROW_LENGTH, 0);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, frame);
    t.key = '';
  }
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

function linkProgram(gl: WebGL2RenderingContext, vertex: string, fragment: string): WebGLProgram {
  const compile = (type: number, source: string) => {
    const shader = gl.createShader(type)!;
    gl.shaderSource(shader, source);
    gl.compileShader(shader);
    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
      throw new AlphaVideoError('WEBGL_UNSUPPORTED', `Shader: ${gl.getShaderInfoLog(shader)}`);
    }
    return shader;
  };
  const program = gl.createProgram()!;
  gl.attachShader(program, compile(gl.VERTEX_SHADER, vertex));
  gl.attachShader(program, compile(gl.FRAGMENT_SHADER, fragment));
  gl.linkProgram(program);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
    throw new AlphaVideoError('WEBGL_UNSUPPORTED', `Program: ${gl.getProgramInfoLog(program)}`);
  }
  return program;
}
