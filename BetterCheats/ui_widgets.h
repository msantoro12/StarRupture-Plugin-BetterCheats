#pragma once

#include "plugin_interface.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>

// Small custom widgets. ResetButton draws a Material Icons glyph (the loader
// embeds MaterialIcons-Regular.ttf and loads glyphs on demand -- see
// MaterialIcons.md, "Plugins need no setup to use these") through the
// ImDrawList API rather than a real Button widget, so its color can react to
// hover/active within the same frame. A real Button widget can't: it draws its
// label with whatever Col_Text is on the style stack at submit time, and
// IsItemHovered() only becomes true *after* that submit, one frame too late to
// feed back into the color that was just drawn.
namespace BetterCheats::UI
{
	// A "replay" glyph button (U+E042, MaterialIcons.md), muted grey at
	// rest and white on hover/active. Returns true on click. `size` is the
	// square side in pixels; pass ImGui's frame height for a control that lines
	// up with a checkbox. `id` is this instance's ImGui identity -- as with any
	// widget sharing a label across instances (see the doc's ##id note), every
	// call site needs its own, which here also still doubles as the click
	// target's InvisibleButton id, same as before.
	inline bool ResetButton(IModLoaderImGui* imgui, const char* id, float size = 0.0f)
	{
		if (size <= 0.0f)
			size = imgui->GetFrameHeight ? imgui->GetFrameHeight() : 18.0f;

		float x = 0.0f, y = 0.0f;
		imgui->GetCursorScreenPos(&x, &y);

		const bool pressed = imgui->InvisibleButton(id, size, size);
		const bool hovered = imgui->IsItemHovered();
		const bool active  = imgui->IsItemActive();

		// ImGui packs colours as 0xAABBGGRR.
		const unsigned int col = (active || hovered) ? 0xFFFFFFFFu    // white while hovered/held
		                                              : 0xFFB0B0B0u;  // muted grey at rest

		PluginDrawList dl = imgui->GetWindowDrawList();
		if (!dl)
			return pressed;

		const char* glyph = "\xEE\x81\x82";   // U+E042 replay
		float textW = 0.0f, textH = 0.0f;
		imgui->CalcTextSize(glyph, &textW, &textH, false, -1.0f);
		imgui->DL_AddText(dl, x + (size - textW) * 0.5f, y + (size - textH) * 0.5f, col, glyph);

		return pressed;
	}

	// "Show live values" readout, shared by the Weapons and Movement panels.
	// `expected` is the value a row intends (the composed result while active, or
	// the game's own value while inactive); `game` is what the attribute/field
	// actually holds right now. They should always converge within a frame or two
	// of a change -- a lasting mismatch means something else keeps overriding or
	// ignoring the write.

	// 2 decimals normally; 3 once the magnitude drops under 0.1 (e.g. a near-zero
	// recoil multiplier), where 2 decimals would hide the difference that matters.
	inline void FormatLiveValue(char* out, size_t outSize, float value)
	{
		const float mag = std::fabs(value);
		snprintf(out, outSize, (mag > 0.0f && mag < 0.1f) ? "%.3f" : "%.2f", value);
	}

	// Match = within display precision: an absolute 0.005, or a relative 1e-4 for
	// values large enough that an absolute epsilon would be too tight (speeds etc.).
	inline bool LiveValuesMatch(float expected, float game)
	{
		const float diff = std::fabs(expected - game);
		if (diff <= 0.005f) return true;
		const float mag = (std::max)(std::fabs(expected), std::fabs(game));
		return mag > 0.0f && (diff / mag) <= 1e-4f;
	}

	// Draws "<expected> = <game>" in blue on a match, "<expected> != <game>" in red
	// otherwise. ASCII "!=" rather than U+2260 -- the loader's baked font ranges
	// (imgui_backend.cpp) stop at Latin-1/Cyrillic plus a CJK merge that doesn't
	// cover Mathematical Operators either, so the real glyph would render as "?".
	inline void RenderLiveValue(IModLoaderImGui* imgui, float expected, float game)
	{
		char e[32], g[32], line[80];
		FormatLiveValue(e, sizeof(e), expected);
		FormatLiveValue(g, sizeof(g), game);
		const bool match = LiveValuesMatch(expected, game);
		snprintf(line, sizeof(line), "%s %s %s", e, match ? "=" : "!=", g);

		if (match) imgui->TextColored(0.310f, 0.639f, 0.878f, 1.0f, line);   // #4FA3E0
		else       imgui->TextColored(0.878f, 0.282f, 0.282f, 1.0f, line);   // #E04848
	}
}
