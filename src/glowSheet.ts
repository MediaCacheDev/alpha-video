/**
 * The arithmetic behind a multi-state glow sheet.
 *
 * A glow export can hold several interaction states in one video, stacked one above another:
 * every frame of the file is a tall image whose rows are `default`, `hover`, `press` in that
 * order, each row being the button plus the margin its glow bleeds into. Showing one state means
 * showing one row and clipping the rest — one file, one decoder, and hover costs nothing.
 *
 * Kept free of React because it is the half worth checking. The layout is a contract with
 * whatever produced the file, and getting a row offset wrong shows hover when you meant press.
 */

/** The order rows are written in. Must match the producer's. */
export const STATE_ORDER = ['default', 'hover', 'press'] as const;

export type GlowState = (typeof STATE_ORDER)[number];

/**
 * Padding around the button on every side, for the glow to spill into.
 *
 * Mirrors `GLOW_CANVAS_MARGIN` in react-native-animated-glow, whose studio produces these
 * sheets. A sheet captured with a different margin must say so, or the label will sit off-centre
 * by the difference.
 */
export const DEFAULT_GLOW_MARGIN = 100;

export interface Size {
    width: number;
    height: number;
}

export interface GlowSheetLayout {
    rows: number;
    margin: number;
    /** The button's own box, which is what it occupies in layout. */
    content: Size;
    /** One row: the content plus the glow margin on every side. */
    frame: Size;
    /** The whole image, as it must be drawn for one row of it to come out the right size. */
    sheet: Size;
}

/**
 * Which row to show for a requested state, given the rows the file actually has.
 *
 * Falls back down the interaction order rather than to nothing: a sheet with no `press` row shows
 * `hover` when pressed, and one with neither shows `default`. A button whose glow simply stopped
 * responding would be worse than one that responds less.
 */
export const rowForState = (states: readonly string[], state: string): number => {
    if (!Array.isArray(states) || states.length === 0) return 0;

    const wanted = (STATE_ORDER as readonly string[]).indexOf(state);
    // Unknown state: treat it as rest rather than guessing.
    if (wanted < 0) return 0;

    for (let index = wanted; index >= 0; index -= 1) {
        const found = states.indexOf(STATE_ORDER[index]);
        if (found >= 0) return found;
    }

    return 0;
};

/**
 * Measures everything the component needs from the button's own size.
 *
 * `content` is what a caller thinks in — the size of the button — and everything else follows
 * from it. The row is the content plus a margin on each side; the sheet is one row per state.
 */
export const glowSheetLayout = (
    content: Size,
    states: readonly string[],
    margin: number = DEFAULT_GLOW_MARGIN,
): GlowSheetLayout => {
    const rows = Math.max(1, Array.isArray(states) ? states.length : 1);
    const frame = {
        width: content.width + margin * 2,
        height: content.height + margin * 2,
    };

    return {
        rows,
        margin,
        content: { width: content.width, height: content.height },
        frame,
        sheet: { width: frame.width, height: frame.height * rows },
    };
};

/**
 * How far up to slide the sheet so that `row` is the one in the window.
 *
 * Negative by construction: row 0 sits at the top and every later row is above the window's top
 * edge until it is pulled down to it.
 */
export const rowOffset = (layout: GlowSheetLayout, row: number): number =>
    -layout.frame.height * Math.max(0, row);

/**
 * The playback rate a row should run at.
 *
 * Every row of a sheet is baked at ONE speed, because rows share a timeline — the producer has no
 * choice, since rows baked at their own speeds agree only at frame 0 and drift apart after, and
 * switching mid-loop would jump by however far they had diverged. The states' own speeds survive
 * as these per-row rates instead, which is a thing a video player can actually apply.
 *
 * Defaults to 1 for anything missing or nonsensical: an absent rate means the producer did not
 * care, and a rate of zero would be a stopped animation rather than a slow one.
 */
export const rateForRow = (rates: readonly number[] | undefined, row: number): number => {
    const rate = rates?.[Math.max(0, row)];
    if (typeof rate !== 'number' || !Number.isFinite(rate) || rate <= 0) return 1;
    return rate;
};
