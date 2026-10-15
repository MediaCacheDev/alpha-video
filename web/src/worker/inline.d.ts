/** A Worker's whole script, bundled into one string by scripts/inline-worker.mjs */
declare module '*?worker' {
  const source: string;
  export default source;
}
