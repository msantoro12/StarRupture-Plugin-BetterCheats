#pragma once

#include "plugin_interface.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>

// Small custom widgets drawn through the ImDrawList API rather than the font.
// The loader's ImGui font has no guaranteed glyph range beyond ASCII -- nothing
// in the plugin ships a non-ASCII UI string -- so an icon like U+21BA cannot be
// relied on to render. These draw the shape directly instead.
namespace BetterCheats::UI
{
	// A circular "revert to default" arrow: a near-full arc with an arrowhead on
	// the leading end. Returns true on click. `size` is the square side in pixels;
	// pass ImGui's frame height for a control that lines up with a checkbox.
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
		const unsigned int col = active  ? 0xFFFFFFFFu     // white while held
		                       : hovered ? 0xFFFFFFFFu
		                                 : 0xFFB0B0B0u;    // muted grey at rest

		PluginDrawList dl = imgui->GetWindowDrawList();
		if (!dl)
			return pressed;

		const float cx = x + size * 0.5f;
		const float cy = y + size * 0.5f;
		const float r  = size * 0.30f;

		// Arc swept anticlockwise, stopping short of a full turn so the gap reads
		// as motion rather than a plain circle.
		const float aMin = 0.70f;
		const float aMax = 5.75f;
		imgui->DL_PathArcTo(dl, cx, cy, r, aMin, aMax, 20);
		imgui->DL_PathStroke(dl, col, 0, size * 0.10f);

		// Arrowhead on the arc's leading end, pointing along the tangent.
		const float ex = cx + r * std::cos(aMax);
		const float ey = cy + r * std::sin(aMax);
		const float tx = -std::sin(aMax);          // unit tangent
		const float ty =  std::cos(aMax);
		const float nx = std::cos(aMax);           // unit normal (outward)
		const float ny = std::sin(aMax);
		const float h  = size * 0.22f;             // head length
		const float w  = size * 0.13f;             // half-width

		imgui->DL_AddTriangleFilled(dl,
			ex + tx * h,          ey + ty * h,
			ex + nx * w,          ey + ny * w,
			ex - nx * w,          ey - ny * w,
			col);

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
