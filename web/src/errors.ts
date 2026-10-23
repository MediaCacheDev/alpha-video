/**
 * - `LOAD_FAILED`: the file couldn't be fetched (network, HTTP status, or CORS for a
 *   cross-origin file outside `<video>`).
 * - `UNSUPPORTED_FILE`: not a WebM with a VP9 or AV1 track.
 * - `NO_ALPHA_PATH`: no engine in this browser can show the file with its alpha.
 * - `DECODER_UNSUPPORTED`: the chosen engine has no decoder for the file's codec.
 * - `DECODE_FAILED`: the decoder reported an error while playing.
 * - `WEBGL_UNSUPPORTED`: no WebGL2 context could be created for the canvas engines.
 * - `NEEDS_SERVICE_WORKER`: only `<video>` can play the file (`decoder: 'browser'`, or a file
 *   without alpha), and it can't load bytes, a ByteSource or a URL with headers without the
 *   range-source Service Worker.
 */
export type AlphaVideoErrorCode =
  | 'LOAD_FAILED'
  | 'UNSUPPORTED_FILE'
  | 'NO_ALPHA_PATH'
  | 'DECODER_UNSUPPORTED'
  | 'DECODE_FAILED'
  | 'WEBGL_UNSUPPORTED'
  | 'NEEDS_SERVICE_WORKER';

/** An error passed to `onError` that callers can tell apart by `code`. */
export class AlphaVideoError extends Error {
  readonly code: AlphaVideoErrorCode;

  constructor(code: AlphaVideoErrorCode, message: string) {
    super(message);
    this.name = 'AlphaVideoError';
    this.code = code;
  }
}
