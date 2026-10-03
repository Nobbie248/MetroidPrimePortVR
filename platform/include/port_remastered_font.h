#pragma once

// Remastered's typeface, as a distance-field font the port can draw (port_hd_font.h).
//
// Remastered has one FONT asset. It holds several faces, each a set of glyph
// boxes on distance-field textures; the first face is the Latin text every
// screen of the original uses, on a single texture.

#include "port_hd_font.h"
#include "port_remastered_cmdl.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace PortRemastered {

// The first face of a FONT asset: its glyphs and the texture they are on.
// `out` has no size or distances yet; the texture gives those (SetFontAtlas).
bool ParseFont(const uint8_t* data, size_t size, ModelUuid& atlas, PortHdFont::Font& out, std::string& error);

// Gives the font its distances: the red channel of the decoded texture.
bool SetFontAtlas(PortHdFont::Font& font, uint32_t width, uint32_t height, const uint8_t* rgba, size_t size);

}  // namespace PortRemastered
