#pragma once

// Remastered's text, as replacements for the disc's string tables.
//
// Remastered keeps its text in MSBT assets: an RFRM form with one chunk per
// language, each holding a standard "MsgStdBn" message file (labels in LBL1,
// UTF-16 text in TXT2). A string that came from the original game is labelled
// "[<STRG id>]_<index>", so the two games' tables line up without a list.
//
// Only strings whose wording changed are taken, and only when the text can be
// written in the original's markup: a colour tag becomes "&main-color", size
// and layout tags are dropped, a line break is kept only where the disc's
// string breaks a line too, and a string naming a button or an icon keeps
// the disc's version, Remastered's controls not being this game's.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace PortRemastered {

struct TextEntry {
  std::string label;
  std::u16string text;  // as stored, tags included
};

// The messages of one language ("USEN") of an MSBT asset.
bool ParseMsbt(const uint8_t* data, size_t size, const char* language, std::vector<TextEntry>& out,
               std::string& error);

// "[0D1F9C75]_002" -> 0x0D1F9C75, 2. False for Remastered's own labels.
bool SplitTextLabel(const std::string& label, uint32_t& strg, uint32_t& index);

// Remastered's string in the original's markup. False when the disc's string
// should stay: the wording is the same, or the text needs something the
// original cannot draw.
bool ConvertText(const std::u16string& remastered, const std::u16string& retail, std::u16string& out);

// A copy of the disc's STRG with the English strings in `strings` (by index)
// replaced where ConvertText allows it. False when the table cannot be read or
// nothing in it changed.
bool MergeStringTable(const uint8_t* retail, size_t size, const std::map<uint32_t, std::u16string>& strings,
                      std::vector<uint8_t>& out, int& changed);

}  // namespace PortRemastered
