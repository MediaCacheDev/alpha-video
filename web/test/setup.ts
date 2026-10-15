// The WebAssembly module is built for browsers only (its node startup code trips up webpack).
// Its .wasm is inlined, so under node it only needs to believe it's on a page.
(globalThis as { window?: unknown }).window ??= globalThis;
