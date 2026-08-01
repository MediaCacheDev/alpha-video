import React, { useMemo, useState } from 'react';
import { Pressable, View } from 'react-native';
import type { PressableProps, StyleProp, ViewStyle } from 'react-native';

import { WebMAlphaView } from './WebMAlphaView';
import type { PlaybackMode, WebMSource } from './WebMAlphaView';
import {
    DEFAULT_GLOW_MARGIN,
    glowSheetLayout,
    rateForRow,
    rowForState,
    rowOffset,
    type Size,
} from './glowSheet';

export interface GlowButtonProps extends Omit<PressableProps, 'style'> {
    source: WebMSource;
    /**
     * The rows the file holds, in order — normally `['default', 'hover', 'press']`, and whatever
     * subset was exported otherwise. The file cannot say this itself: three rows of 290px look
     * exactly like one row of 870.
     */
    states?: readonly string[];
    /**
     * Playback rate per row, matching `states`.
     *
     * Every row is baked at one speed so the rows stay in phase, so this is where a state's own
     * speed went. Applying it is the reason to prefer a video here over an APNG: the rate reaches
     * the decoder's frame clock, which no APNG decoder exposes.
     */
    rates?: readonly number[];
    /** The button's own size, excluding the glow margin. */
    content: Size;
    /** Padding baked into each row for the glow to bleed into. */
    margin?: number;
    /**
     * Identifies the file's CONTENTS, not its location.
     *
     * The player caches by URI, which is wrong for a file rewritten at the same path — a second
     * export of the same states would go on showing the first.
     */
    cacheKey?: string;
    /** Pins the shown row, ignoring the pointer. Useful for previews and for tests. */
    activeState?: string;
    paused?: boolean;
    /** Passed through; `preload` is right for a short loop and is the player's default. */
    mode?: PlaybackMode;
    /** Applied to the content box, so a caller can add a border radius or a background. */
    style?: StyleProp<ViewStyle>;
    children?: React.ReactNode;
}

/**
 * A button whose glow is a pre-rendered animation rather than a live shader.
 *
 * Takes one WebM holding every interaction state stacked in rows — what
 * react-native-animated-glow's studio exports when more than one state is chosen — and shows the
 * row matching what the pointer is doing. Hover and press therefore cost nothing at all: no
 * second file to fetch, no second decoder, and no Skia on the consuming side.
 *
 * The button occupies its CONTENT box in layout, and the glow spills outside it. That is the whole
 * point of the margin in the exported frame, and why the visible window is the padded row rather
 * than the content: clipping to the content box would cut the glow off at the button's edge.
 * Neighbouring rows cannot leak in, because each was captured clamped to its own canvas.
 *
 * Switching rows never restarts the video. The source, the cache key and the decoder are all
 * unchanged by a hover — only the offset and the rate move — so the animation carries on from
 * where it was, which is what makes the change read as the button reacting rather than resetting.
 */
export const GlowButton: React.FC<GlowButtonProps> = ({
    source,
    states = ['default'],
    rates,
    content,
    margin = DEFAULT_GLOW_MARGIN,
    cacheKey,
    activeState,
    onPress,
    disabled = false,
    paused = false,
    mode,
    style,
    children,
    ...rest
}) => {
    const [hovered, setHovered] = useState(false);
    const [pressed, setPressed] = useState(false);

    const layout = useMemo(
        () => glowSheetLayout(content, states, margin),
        [content, states, margin],
    );

    // Derived from the two booleans rather than tracked as one value: releasing a press while
    // still hovering has to land on hover, and a single "pointer state" gets that wrong depending
    // on which handler ran last.
    const pointerState = pressed ? 'press' : hovered ? 'hover' : 'default';
    const row = rowForState(states, activeState ?? (disabled ? 'default' : pointerState));

    return (
        <Pressable
            accessibilityRole="button"
            disabled={disabled}
            onPress={onPress}
            onPressIn={() => setPressed(true)}
            onPressOut={() => setPressed(false)}
            onHoverIn={() => setHovered(true)}
            onHoverOut={() => setHovered(false)}
            style={[
                {
                    width: layout.content.width,
                    height: layout.content.height,
                    alignItems: 'center',
                    justifyContent: 'center',
                },
                style,
            ]}
            {...rest}
        >
            {/*
              The window onto one row. Absolutely positioned and inset by the margin so the row
              lands centred on the content box, with the bleed hanging outside it — the button
              itself stays the size it claims to be in layout.

              `overflow: 'hidden'` is what hides the other states, and is required explicitly on
              Android, where children draw outside their parent by default.
            */}
            <View
                pointerEvents="none"
                testID="glow-sheet-window"
                style={{
                    position: 'absolute',
                    left: -layout.margin,
                    top: -layout.margin,
                    width: layout.frame.width,
                    height: layout.frame.height,
                    overflow: 'hidden',
                }}
            >
                <WebMAlphaView
                    testID="glow-sheet-video"
                    source={source}
                    cacheKey={cacheKey}
                    paused={paused}
                    loop
                    autoPlay
                    mode={mode}
                    // Where the state's own speed comes back. The player changes its frame
                    // interval and leaves the playhead alone, so this accelerates the animation
                    // without seeking it — the same thing the live shader does, which integrates
                    // phase and so never jumps when the speed changes.
                    playbackSpeed={rateForRow(rates, row)}
                    style={{
                        position: 'absolute',
                        left: 0,
                        // Slid by layout rather than by transform, so the parent's clip applies
                        // to it on every platform without depending on draw-time ordering.
                        top: rowOffset(layout, row),
                        // Drawn at exactly the size the arithmetic says. The sheet's aspect ratio
                        // is the row's by construction, so there is nothing to fit and nothing to
                        // letterbox.
                        width: layout.sheet.width,
                        height: layout.sheet.height,
                    }}
                />
            </View>

            {/*
              The label, over the glow. Later siblings paint on top in React Native, and the
              window above is `pointerEvents: 'none'`, so the press still belongs to the button.
            */}
            {children}
        </Pressable>
    );
};

GlowButton.displayName = 'GlowButton';

export default GlowButton;
