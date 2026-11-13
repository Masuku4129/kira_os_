#ifndef THEME_H
#define THEME_H

/*
 * Semantic design tokens shared by the shell and built-in applications.
 * KIRA_SPATIAL_PREVIEW opts into the approved Spatial Minimal visual system.
 * The default branch keeps the Legacy palette unchanged until Spatial is
 * accepted as the default layout.
 */

#define TH_SPACE_1 4
#define TH_SPACE_2 8
#define TH_SPACE_3 12
#define TH_SPACE_4 16
#define TH_SPACE_6 24
#define TH_BAR_H 24

#ifdef KIRA_SPATIAL_PREVIEW

/* Spatial Minimal: canvas, layered surfaces and controls. */
#define TH_BG0                 0x00070D19u
#define TH_BG1                 0x000A1222u
#define TH_BAR0                0x000C1629u
#define TH_BAR1                0x00070D19u
#define TH_PANEL0              0x00101C31u
#define TH_PANEL1              0x000C1629u
#define TH_CONTENT             0x00070D19u
#define TH_TILE                0x0013213Au
#define TH_BORDER              0x00263653u
#define TH_GRID                0x00263653u

/* Brand accents and semantic states. */
#define TH_ACCENT_PRIMARY      0x009B6BFFu
#define TH_ACCENT_SECONDARY    0x0042A5FFu
#define TH_STATE_SUCCESS       0x0035C995u
#define TH_STATE_WARNING       0x00F2B84Bu
#define TH_STATE_DANGER        0x00F07178u
#define TH_CYAN                TH_ACCENT_PRIMARY
#define TH_BLUE                TH_ACCENT_SECONDARY
#define TH_PURPLE              TH_ACCENT_PRIMARY
#define TH_GREEN               TH_STATE_SUCCESS
#define TH_AMBER               TH_STATE_WARNING
#define TH_RED                 TH_STATE_DANGER
#define TH_PINK                0x00C58BFAu

/* Text hierarchy. */
#define TH_TEXT                0x00F1F3FAu
#define TH_TEXT_DIM            0x00AAB5CCu
#define TH_TEXT_FAINT          0x0071809Au

/* Shared application surfaces. */
#define TH_CARD                0x000C1629u
#define TH_CARD_EDGE           0x00263653u
#define TH_TRACK                0x0013213Au
#define TH_ROW_ALT             0x00091222u

/* Geometry and special surfaces. */
#define TH_RADIUS_CONTROL      12
#define TH_RADIUS_SURFACE      20
#define TH_GAP                 TH_SPACE_3
#define TH_FULLSCREEN_BG       TH_BG0
#define TH_METER_TRACK         TH_TRACK
#define TH_CURSOR_EDGE         TH_BG0
#define TH_RESIZE_IDLE         TH_TEXT_FAINT
#define TH_MIND_ORB_BG         TH_CONTENT

#else

/* Legacy palette — intentionally preserved for non-preview builds. */
#define TH_BG0        0x00050A16u
#define TH_BG1        0x000B1630u
#define TH_BAR0       0x00081022u
#define TH_BAR1       0x00060C1Au
#define TH_PANEL0     0x000C1629u
#define TH_PANEL1     0x00091120u
#define TH_CONTENT    0x00070D19u
#define TH_TILE       0x000C1830u
#define TH_BORDER     0x001C2C48u
#define TH_GRID       0x0022D3EEu

#define TH_CYAN       0x0022D3EEu
#define TH_BLUE       0x003B82F6u
#define TH_PURPLE     0x00A855F7u
#define TH_GREEN      0x0034D399u
#define TH_AMBER      0x00FBBF24u
#define TH_RED        0x00F87171u
#define TH_PINK       0x00F472B6u

#define TH_TEXT       0x00E2E8F0u
#define TH_TEXT_DIM   0x008DA2C0u
#define TH_TEXT_FAINT 0x00566B8Eu

#define TH_CARD       0x000B1528u
#define TH_CARD_EDGE  0x00182844u
#define TH_TRACK      0x00132139u
#define TH_ROW_ALT    0x00091426u

#define TH_RADIUS_CONTROL 8
#define TH_RADIUS_SURFACE 10
#define TH_GAP 10
#define TH_FULLSCREEN_BG 0x0004070Eu
#define TH_METER_TRACK 0x00142238u
#define TH_CURSOR_EDGE 0x00051018u
#define TH_RESIZE_IDLE 0x0035557Au
#define TH_MIND_ORB_BG 0x00060B16u

#endif

/* Compatibility name for shared card renderers. */
#define TH_RADIUS TH_RADIUS_SURFACE

/* Window actions remain explicit and distinguishable in both layouts. */
#define TH_CLOSE 0x00FF5F57u
#define TH_MIN   0x00FEBC2Eu
#define TH_FULL  0x0028C840u

#endif
