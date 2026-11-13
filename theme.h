#ifndef THEME_H
#define THEME_H

/*
 * KIRA Spatial Minimal — semantic design tokens.
 * Palette from docs/UI1_SPATIAL_LAYOUT_CONTRACT.md.
 * Legacy TH_CYAN/TH_BLUE/TH_PURPLE names remain aliases so existing
 * applications migrate without changing their rendering contracts.
 */

/* Canvas and layered surfaces */
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

/* Brand accents and semantic states */
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

/* Text hierarchy */
#define TH_TEXT                0x00F1F3FAu
#define TH_TEXT_DIM            0x00AAB5CCu
#define TH_TEXT_FAINT          0x0071809Au

/* Shared application surfaces */
#define TH_CARD                0x000C1629u
#define TH_CARD_EDGE           0x00263653u
#define TH_TRACK               0x0013213Au
#define TH_ROW_ALT             0x00091222u

/* Geometry: 4/8/12/16/24 spacing scale, 12 px controls, 20 px surfaces. */
#define TH_SPACE_1             4
#define TH_SPACE_2             8
#define TH_SPACE_3             12
#define TH_SPACE_4             16
#define TH_SPACE_6             24
#define TH_RADIUS_CONTROL      12
#define TH_RADIUS_SURFACE      20
#define TH_BAR_H               24
#define TH_GAP                 TH_SPACE_3
#define TH_RADIUS              TH_RADIUS_SURFACE

/* Window actions retain explicit, distinguishable states. */
#define TH_CLOSE               0x00F07178u
#define TH_MIN                 0x00F2B84Bu
#define TH_FULL                0x0035C995u

#endif
