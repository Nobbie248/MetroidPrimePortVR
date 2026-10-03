#include "port_remastered_text.h"

#include <algorithm>
#include <cstring>

namespace PortRemastered {
namespace {

constexpr uint32_t kStrgMagic = 0x87654321;
constexpr uint32_t kEnglish = 0x454E474C;  // 'ENGL'

uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t Le64(const uint8_t* p) { return uint64_t(Le32(p)) | uint64_t(Le32(p + 4)) << 32; }
uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]); }

void PutBe32(std::vector<uint8_t>& out, uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(uint8_t(value >> shift));
  }
}

// One "MsgStdBn" file: a 32 byte header, then sections of a 16 byte header
// (name, size) and their data, each padded to 16 bytes.
bool ParseMessages(const uint8_t* data, size_t size, std::vector<TextEntry>& out, std::string& error) {
  if (size < 0x20 || std::memcmp(data, "MsgStdBn", 8) != 0 || data[8] != 0xFF || data[9] != 0xFE || data[12] != 1) {
    error = "not a little endian UTF-16 message file";
    return false;
  }
  const uint8_t* labels = nullptr;
  const uint8_t* texts = nullptr;
  size_t labelsSize = 0;
  size_t textsSize = 0;
  for (size_t at = 0x20; at + 16 <= size;) {
    const size_t length = Le32(data + at + 4);
    if (length > size - at - 16) {
      error = "a section runs past the end";
      return false;
    }
    if (std::memcmp(data + at, "LBL1", 4) == 0) {
      labels = data + at + 16;
      labelsSize = length;
    } else if (std::memcmp(data + at, "TXT2", 4) == 0) {
      texts = data + at + 16;
      textsSize = length;
    }
    at = (at + 16 + length + 15) & ~size_t(15);
  }
  if (labels == nullptr || texts == nullptr || labelsSize < 4 || textsSize < 4) {
    error = "no labels or no text";
    return false;
  }
  const size_t count = Le32(texts);
  if (count > (textsSize - 4) / 4) {
    error = "the text table is cut short";
    return false;
  }
  const size_t first = out.size();
  out.resize(first + count);
  for (size_t i = 0; i < count; ++i) {
    const size_t begin = Le32(texts + 4 + 4 * i);
    const size_t end = i + 1 < count ? Le32(texts + 8 + 4 * i) : textsSize;
    if (begin > end || end > textsSize) {
      error = "a text offset is out of range";
      return false;
    }
    std::u16string& text = out[first + i].text;
    for (size_t p = begin; p + 2 <= end; p += 2) {
      text.push_back(char16_t(Le16(texts + p)));
    }
    // The terminator; a zero inside a tag's payload is not one, so only the last counts.
    if (!text.empty() && text.back() == 0) {
      text.pop_back();
    }
  }
  // Hash slots of (count, offset), each a run of: length, name, text index.
  const size_t slots = Le32(labels);
  if (slots > (labelsSize - 4) / 8) {
    error = "the label table is cut short";
    return false;
  }
  for (size_t s = 0; s < slots; ++s) {
    const size_t labelCount = Le32(labels + 4 + 8 * s);
    size_t at = Le32(labels + 8 + 8 * s);
    for (size_t l = 0; l < labelCount; ++l) {
      if (at >= labelsSize || labels[at] + size_t(5) > labelsSize - at) {
        error = "a label is out of range";
        return false;
      }
      const size_t length = labels[at];
      const size_t index = Le32(labels + at + 1 + length);
      if (index < count) {
        out[first + index].label.assign(reinterpret_cast<const char*>(labels + at + 1), length);
      }
      at += 5 + length;
    }
  }
  return true;
}

bool IsSpace(char16_t c) { return c == u' ' || c == u'\n' || c == u'\r' || c == u'\t'; }

// The words alone: tags out, runs of white space as one space.
std::u16string Words(const std::u16string& text, bool retailMarkup) {
  std::u16string out;
  bool space = false;
  for (size_t i = 0; i < text.size(); ++i) {
    if (retailMarkup && text[i] == u'&') {
      const size_t end = text.find(u';', i);
      if (end != std::u16string::npos) {
        i = end;
        continue;
      }
    }
    if (IsSpace(text[i])) {
      space = !out.empty();
      continue;
    }
    if (space) {
      out.push_back(u' ');
      space = false;
    }
    out.push_back(text[i]);
  }
  return out;
}

// The last word of a text, trailing white space ignored.
std::u16string LastWord(const std::u16string& text) {
  size_t end = text.size();
  while (end > 0 && IsSpace(text[end - 1])) {
    --end;
  }
  size_t begin = end;
  while (begin > 0 && !IsSpace(text[begin - 1])) {
    --begin;
  }
  return text.substr(begin, end - begin);
}

// The words a retail string ends its lines with. Remastered breaks its lines
// by hand to fit its own boxes; only a break after one of these words belongs
// to the text ("Morphology: <name>\n", a log's heading), the rest are left to
// the original's word wrap.
std::vector<std::u16string> LineEnds(const std::u16string& retail) {
  std::vector<std::u16string> ends;
  std::u16string text;
  for (size_t i = 0; i < retail.size(); ++i) {
    if (retail[i] == u'&') {
      const size_t end = retail.find(u';', i);
      if (end != std::u16string::npos) {
        i = end;
        continue;
      }
    }
    if (retail[i] == u'\n') {
      const std::u16string word = LastWord(text);
      if (!word.empty()) {
        ends.push_back(word);
      }
    }
    text.push_back(retail[i]);
  }
  return ends;
}

bool StartsWith(const std::u16string& text, size_t at, const char16_t* prefix) {
  return text.compare(at, std::char_traits<char16_t>::length(prefix), prefix) == 0;
}

// The tags a retail string opens with that say how its widget lays text out,
// which Remastered leaves to the widget itself.
std::u16string LayoutPrefix(const std::u16string& retail) {
  static const char16_t* const kLayout[] = {u"&just=", u"&vjust=", u"&font=", u"&line-spacing=",
                                            u"&line-extra-space="};
  size_t at = 0;
  while (at < retail.size() && retail[at] == u'&') {
    bool layout = false;
    for (const char16_t* tag : kLayout) {
      layout = layout || StartsWith(retail, at, tag);
    }
    const size_t end = retail.find(u';', at);
    if (!layout || end == std::u16string::npos) {
      break;
    }
    at = end + 1;
  }
  return retail.substr(0, at);
}

}  // namespace

bool ParseMsbt(const uint8_t* data, size_t size, const char* language, std::vector<TextEntry>& out,
               std::string& error) {
  if (size < 0x20 || std::memcmp(data, "RFRM", 4) != 0 || std::memcmp(data + 0x14, "MSBT", 4) != 0) {
    error = "not an MSBT";
    return false;
  }
  // The form's length leaves out its header; what follows it is the extractor's footer.
  const uint64_t form = Le64(data + 4);
  const size_t end = form < size - 0x20 ? size_t(form) + 0x20 : size;
  // Chunks of a 24 byte header: the language, the length, a version and a skip.
  for (size_t at = 0x20; at + 24 <= end;) {
    const uint64_t length = Le64(data + at + 4);
    if (length > end - at - 24) {
      error = "a language runs past the end";
      return false;
    }
    if (std::memcmp(data + at, language, 4) == 0) {
      return ParseMessages(data + at + 24, size_t(length), out, error);
    }
    at += 24 + size_t(length);
  }
  error = std::string("no ") + language + " text";
  return false;
}

bool SplitTextLabel(const std::string& label, uint32_t& strg, uint32_t& index) {
  if (label.size() < 12 || label.size() > 16 || label[0] != '[' || label[9] != ']' || label[10] != '_') {
    return false;
  }
  strg = 0;
  for (size_t i = 1; i < 9; ++i) {
    const char c = label[i];
    const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (digit < 0) {
      return false;
    }
    strg = strg << 4 | uint32_t(digit);
  }
  index = 0;
  for (size_t i = 11; i < label.size(); ++i) {
    if (label[i] < '0' || label[i] > '9') {
      return false;
    }
    index = index * 10 + uint32_t(label[i] - '0');
  }
  return true;
}

bool ConvertText(const std::u16string& remastered, const std::u16string& retail, std::u16string& out) {
  static const char16_t kHex[] = u"0123456789ABCDEF";
  std::u16string body;
  std::u16string plain;
  bool coloured = false;
  const std::vector<std::u16string> lineEnds = LineEnds(retail);
  for (size_t i = 0; i < remastered.size(); ++i) {
    const char16_t c = remastered[i];
    if (c == 0x0E) {
      // group, type, payload bytes, payload
      if (i + 3 >= remastered.size()) {
        return false;
      }
      const unsigned group = remastered[i + 1];
      const unsigned type = remastered[i + 2];
      const size_t units = (size_t(remastered[i + 3]) + 1) / 2;
      if (units > remastered.size() - i - 4) {
        return false;
      }
      const char16_t* payload = remastered.data() + i + 4;
      i += 3 + units;
      if (group == 0 && type == 3 && units == 2) {
        // A colour, as bytes R G B A. Opaque black ends every highlighted run,
        // on dark backgrounds too: it gives the text its widget's colour back.
        if (coloured) {
          body += u"&pop;";
          coloured = false;
        }
        if (payload[0] != 0 || payload[1] != 0xFF00) {
          body += u"&push;&main-color=#";
          for (int k = 0; k < 2; ++k) {
            const unsigned low = payload[k] & 0xFF;
            const unsigned high = payload[k] >> 8;
            body += {kHex[low >> 4], kHex[low & 15], kHex[high >> 4], kHex[high & 15]};
          }
          body += u';';
          coloured = true;
        }
      } else if ((group == 0 && type == 2) || (group == 1 && type == 3)) {
        // A size in percent, and a layout mode: the original's widgets have their own.
      } else {
        return false;  // a button of Remastered's controls, or one of its icons
      }
    } else if (c == 0x0F) {
      i += 2;  // a closing tag: group, type
    } else if (c == u'\n') {
      if (std::find(lineEnds.begin(), lineEnds.end(), LastWord(plain)) != lineEnds.end()) {
        body.push_back(c);
        plain.push_back(c);
      } else if (!plain.empty() && !IsSpace(plain.back()) && plain.back() != u'-') {
        // A word broken at its hyphen ("mass-\nproduction") stays joined.
        body.push_back(u' ');
        plain.push_back(u' ');
      }
    } else if (c >= 0x20 && c < 0x7F && c != u'&') {
      body.push_back(c);
      plain.push_back(c);
    } else {
      return false;  // a glyph the original's fonts may lack, or the start of a tag
    }
  }
  if (coloured) {
    body += u"&pop;";
  }
  if (Words(plain, false) == Words(retail, true)) {
    return false;
  }
  out = LayoutPrefix(retail) + body;
  return true;
}

bool MergeStringTable(const uint8_t* retail, size_t size, const std::map<uint32_t, std::u16string>& strings,
                      std::vector<uint8_t>& out, int& changed) {
  changed = 0;
  if (size < 16 || Be32(retail) != kStrgMagic || Be32(retail + 4) != 0) {
    return false;
  }
  const size_t languages = Be32(retail + 8);
  const size_t count = Be32(retail + 12);
  if (languages > (size - 16) / 8 || count > size / 4) {
    return false;
  }
  const size_t base = 16 + 8 * languages;
  // Every language, as its strings.
  std::vector<std::vector<std::u16string>> tables(languages);
  for (size_t l = 0; l < languages; ++l) {
    const size_t offset = Be32(retail + 20 + 8 * l);
    if (offset > size - base || 4 + 4 * count > size - base - offset) {
      return false;
    }
    const uint8_t* table = retail + base + offset + 4;
    const size_t room = size - base - offset - 4;
    for (size_t s = 0; s < count; ++s) {
      std::u16string text;
      for (size_t p = Be32(table + 4 * s);; p += 2) {
        if (p + 2 > room) {
          return false;
        }
        const char16_t c = char16_t(table[p] << 8 | table[p + 1]);
        if (c == 0) {
          break;
        }
        text.push_back(c);
      }
      tables[l].push_back(std::move(text));
    }
    if (Be32(retail + 16 + 8 * l) != kEnglish) {
      continue;
    }
    for (const auto& [index, text] : strings) {
      std::u16string converted;
      if (index < count && ConvertText(text, tables[l][index], converted)) {
        tables[l][index] = std::move(converted);
        ++changed;
      }
    }
  }
  if (changed == 0) {
    return false;
  }
  out.clear();
  PutBe32(out, kStrgMagic);
  PutBe32(out, 0);
  PutBe32(out, uint32_t(languages));
  PutBe32(out, uint32_t(count));
  size_t offset = 0;
  for (size_t l = 0; l < languages; ++l) {
    PutBe32(out, Be32(retail + 16 + 8 * l));
    PutBe32(out, uint32_t(offset));
    offset += 4 + 4 * count;
    for (const std::u16string& text : tables[l]) {
      offset += 2 * (text.size() + 1);
    }
  }
  for (size_t l = 0; l < languages; ++l) {
    size_t length = 4 * count;
    for (const std::u16string& text : tables[l]) {
      length += 2 * (text.size() + 1);
    }
    PutBe32(out, uint32_t(length));
    size_t at = 4 * count;
    for (const std::u16string& text : tables[l]) {
      PutBe32(out, uint32_t(at));
      at += 2 * (text.size() + 1);
    }
    for (const std::u16string& text : tables[l]) {
      for (const char16_t c : text) {
        out.push_back(uint8_t(c >> 8));
        out.push_back(uint8_t(c));
      }
      out.push_back(0);
      out.push_back(0);
    }
  }
  // A resource in a pak is a whole number of 32 byte blocks.
  out.resize((out.size() + 31) & ~size_t(31), 0xFF);
  return true;
}

}  // namespace PortRemastered
