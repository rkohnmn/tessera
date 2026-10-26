// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · theme.hpp
//  A single, deliberate dark palette. Backgrounds are explicit rather than the
//  terminal default so the grid, bars and overlays always read as one surface —
//  the same decision btop and k9s make, and the reason this looks designed on
//  any terminal instead of only on the author's.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "tui/screen.hpp"

namespace tess::tui::theme {

inline constexpr int kPageBg = 233;      ///< #121212 canvas
inline constexpr int kPanelBg = 236;     ///< #303030 raised surfaces
inline constexpr int kBarBg = 235;       ///< #262626 bars
inline constexpr int kSelectionBg = 237; ///< #3a3a3a current row
inline constexpr int kCursorBg = 24;     ///< deep blue, current cell

inline constexpr int kAccent = 45;     ///< cyan
inline constexpr int kAccentAlt = 213; ///< pink
inline constexpr int kText = 252;
inline constexpr int kStrong = 255;
inline constexpr int kMuted = 245;
inline constexpr int kFaint = 241;
inline constexpr int kBorder = 238;
inline constexpr int kCursorFg = 195;

inline constexpr int kNumeric = 114;  ///< green
inline constexpr int kTemporal = 110; ///< blue
inline constexpr int kBoolean = 176;  ///< purple
inline constexpr int kNull = 242;
inline constexpr int kError = 203;
inline constexpr int kSuccess = 114;
inline constexpr int kWarn = 215;
inline constexpr int kSearchHitBg = 58;
inline constexpr int kSearchHitFg = 231;

inline Style base() { return Style::fg_bg(kText, kPageBg); }
inline Style muted() { return Style::fg_bg(kMuted, kPageBg); }
inline Style faint() { return Style::fg_bg(kFaint, kPageBg); }
inline Style accent() { return Style::fg_bg(kAccent, kPageBg); }
inline Style border() { return Style::fg_bg(kBorder, kPageBg); }
inline Style error() { return Style::fg_bg(kError, kPageBg); }
inline Style success() { return Style::fg_bg(kSuccess, kPageBg); }
inline Style warn() { return Style::fg_bg(kWarn, kPageBg); }
inline Style bar() { return Style::fg_bg(kText, kBarBg); }

} // namespace tess::tui::theme
