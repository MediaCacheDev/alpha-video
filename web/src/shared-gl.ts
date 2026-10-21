/**
 * The one WebGL2 context every canvas player draws with. Browsers allow about 16 contexts on a
 * page and drop the oldest past that, so each player draws into this hidden canvas and copies the
 * picture into its own 2D canvas.
 */
import { AlphaVideoError } from './errors';

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

const SAMPLERS = ['uY', 'uU', 'uV', 'uA'];
const UNIFORMS = [...SAMPLERS, 'uMatrix', 'uColorMode', 'uAlphaMode', 'uColorScale', 'uAlphaScale'];

export class SharedGL {
  private static current: SharedGL | null = null;

  readonly canvas: HTMLCanvasElement;
  readonly gl: WebGL2RenderingContext;
  program!: WebGLProgram;
  uniforms!: Record<string, WebGLUniformLocation | null>;
  /** EXT_texture_norm16: 10- and 12-bit planes upload as 16-bit textures */
  norm16 = false;
  lost = false;
  /** Goes up each time the context comes back after a loss: textures made before then are gone */
  generation = 0;
  private users = 0;

  /** The page's context, created for its first player. Throws WEBGL_UNSUPPORTED without WebGL2. */
  static acquire(): SharedGL {
    const shared = (SharedGL.current ??= new SharedGL());
    shared.users++;
    return shared;
  }

  private constructor() {
    this.canvas = document.createElement('canvas');
    const gl = this.canvas.getContext('webgl2', {
      alpha: true,
      premultipliedAlpha: true,
      antialias: false,
      depth: false,
      stencil: false,
      preserveDrawingBuffer: false,
    });
    if (!gl) throw new AlphaVideoError('WEBGL_UNSUPPORTED', 'WebGL2 is unavailable');
    this.gl = gl;
    this.setUp();
    this.canvas.addEventListener('webglcontextlost', this.onLost);
    this.canvas.addEventListener('webglcontextrestored', this.onRestored);
  }

  /**
   * Called by each player that acquired it; the last one frees the drawing buffer. The context
   * stays for the page: WebKit counts contexts given up with loseContext() until they're
   * collected, so one per batch of players would still reach its limit.
   */
  release(): void {
    if (--this.users > 0) return;
    this.canvas.width = 1;
    this.canvas.height = 1;
  }

  /**
   * Makes the drawing buffer at least `width` × `height`. It only grows, so players of different
   * sizes don't reallocate it on every frame.
   */
  fit(width: number, height: number): void {
    if (width > this.canvas.width) this.canvas.width = width;
    if (height > this.canvas.height) this.canvas.height = height;
  }

  private onLost = (e: Event) => {
    e.preventDefault();
    this.lost = true;
  };

  private onRestored = () => {
    this.lost = false;
    this.generation++;
    this.setUp();
  };

  private setUp(): void {
    const gl = this.gl;
    this.norm16 = !!gl.getExtension('EXT_texture_norm16');
    this.program = linkProgram(gl, VERTEX, FRAGMENT);
    this.uniforms = Object.fromEntries(UNIFORMS.map((n) => [n, gl.getUniformLocation(this.program, n)]));
    gl.useProgram(this.program);
    SAMPLERS.forEach((n, unit) => gl.uniform1i(this.uniforms[n], unit));
  }
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
