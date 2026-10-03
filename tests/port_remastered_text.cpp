#include "port_remastered_text.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace PortRemastered;

namespace {
int sFailures = 0;

void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++sFailures;
  }
}

void Put32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(uint8_t(value >> (i * 8)));
  }
}

void PutBe32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 3; i >= 0; --i) {
    out.push_back(uint8_t(value >> (i * 8)));
  }
}

void Pad16(std::vector<uint8_t>& out) {
  while (out.size() % 16 != 0) {
    out.push_back(0xAB);
  }
}

void PutSection(std::vector<uint8_t>& out, const char* name, const std::vector<uint8_t>& data) {
  out.insert(out.end(), name, name + 4);
  Put32(out, uint32_t(data.size()));
  out.resize(out.size() + 8);
  out.insert(out.end(), data.begin(), data.end());
  Pad16(out);
}

// A message file with one hash slot holding every label.
std::vector<uint8_t> Messages(const std::vector<std::pair<std::string, std::u16string>>& entries) {
  std::vector<uint8_t> labels;
  Put32(labels, 1);
  Put32(labels, uint32_t(entries.size()));
  Put32(labels, 12);
  // Labels stored in reverse, to show the index is what orders them.
  for (size_t i = entries.size(); i-- > 0;) {
    labels.push_back(uint8_t(entries[i].first.size()));
    labels.insert(labels.end(), entries[i].first.begin(), entries[i].first.end());
    Put32(labels, uint32_t(i));
  }
  std::vector<uint8_t> texts;
  Put32(texts, uint32_t(entries.size()));
  size_t offset = 4 + 4 * entries.size();
  for (const auto& entry : entries) {
    Put32(texts, uint32_t(offset));
    offset += 2 * (entry.second.size() + 1);
  }
  for (const auto& entry : entries) {
    for (const char16_t c : entry.second) {
      texts.push_back(uint8_t(c));
      texts.push_back(uint8_t(c >> 8));
    }
    texts.push_back(0);
    texts.push_back(0);
  }
  std::vector<uint8_t> out;
  out.insert(out.end(), {'M', 's', 'g', 'S', 't', 'd', 'B', 'n', 0xFF, 0xFE, 0, 0, 1, 3, 2, 0});
  out.resize(0x20);
  PutSection(out, "LBL1", labels);
  PutSection(out, "TXT2", texts);
  return out;
}

std::vector<uint8_t> Msbt(const std::vector<std::pair<const char*, std::vector<uint8_t>>>& languages) {
  std::vector<uint8_t> out = {'R', 'F', 'R', 'M'};
  out.resize(0x14);
  out.insert(out.end(), {'M', 'S', 'B', 'T'});
  out.resize(0x20);
  for (const auto& [name, file] : languages) {
    out.insert(out.end(), name, name + 4);
    Put32(out, uint32_t(file.size()));
    Put32(out, 0);
    Put32(out, 1);
    out.resize(out.size() + 8);
    out.insert(out.end(), file.begin(), file.end());
  }
  const uint32_t form = uint32_t(out.size() - 0x20);
  std::memcpy(out.data() + 4, &form, 4);  // the tests run little endian
  out.insert(out.end(), {'F', 'O', 'O', 'T', 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20});
  return out;
}

std::vector<uint8_t> Strg(const std::vector<std::pair<uint32_t, std::vector<std::u16string>>>& languages) {
  std::vector<uint8_t> out;
  PutBe32(out, 0x87654321);
  PutBe32(out, 0);
  PutBe32(out, uint32_t(languages.size()));
  const size_t count = languages[0].second.size();
  PutBe32(out, uint32_t(count));
  std::vector<uint8_t> body;
  for (const auto& [name, strings] : languages) {
    PutBe32(out, name);
    PutBe32(out, uint32_t(body.size()));
    size_t length = 4 * count;
    for (const std::u16string& s : strings) {
      length += 2 * (s.size() + 1);
    }
    PutBe32(body, uint32_t(length));
    size_t at = 4 * count;
    for (const std::u16string& s : strings) {
      PutBe32(body, uint32_t(at));
      at += 2 * (s.size() + 1);
    }
    for (const std::u16string& s : strings) {
      for (const char16_t c : s) {
        body.push_back(uint8_t(c >> 8));
        body.push_back(uint8_t(c));
      }
      body.push_back(0);
      body.push_back(0);
    }
  }
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

// A tag: group, type, and its payload as 16 bit units.
std::u16string Tag(unsigned group, unsigned type, std::u16string payload) {
  std::u16string out = {char16_t(0x0E), char16_t(group), char16_t(type), char16_t(payload.size() * 2)};
  return out + payload;
}
const std::u16string kRed = Tag(0, 3, {char16_t(0x0AFF), char16_t(0xFF0A)});    // FF 0A 0A FF
const std::u16string kReset = Tag(0, 3, {char16_t(0x0000), char16_t(0xFF00)});  // 00 00 00 FF

void TestMsbt() {
  const std::u16string tagged = u"a " + kReset + u"b";  // a zero unit inside the text
  const std::vector<uint8_t> english = Messages({{"[0000ABCD]_002", u"first"}, {"Named", tagged}});
  const std::vector<uint8_t> french = Messages({{"[0000ABCD]_002", u"premier"}});
  const std::vector<uint8_t> file = Msbt({{"EUFR", french}, {"USEN", english}});

  std::vector<TextEntry> entries;
  std::string error;
  Check(ParseMsbt(file.data(), file.size(), "USEN", entries, error), "the second language is found");
  Check(entries.size() == 2, "both messages are read");
  if (entries.size() == 2) {
    Check(entries[0].label == "[0000ABCD]_002" && entries[0].text == u"first", "a label goes with its index");
    Check(entries[1].label == "Named" && entries[1].text == tagged, "a zero in a tag does not end the text");
  }
  entries.clear();
  Check(ParseMsbt(file.data(), file.size(), "EUFR", entries, error) && entries.size() == 1 &&
            entries[0].text == u"premier",
        "the first language is found");
  entries.clear();
  Check(!ParseMsbt(file.data(), file.size(), "JPJP", entries, error), "a missing language is an error");
  for (size_t cut = 0; cut < file.size(); cut += 7) {
    entries.clear();
    ParseMsbt(file.data(), cut, "USEN", entries, error);  // must not read past `cut`
  }

  uint32_t strg = 0;
  uint32_t index = 0;
  Check(SplitTextLabel("[0000ABCD]_002", strg, index) && strg == 0xABCD && index == 2, "a table label splits");
  Check(!SplitTextLabel("[0000ABCD]_Name", strg, index), "a named label is Remastered's own");
  Check(!SplitTextLabel("MapLegend", strg, index), "a plain label is Remastered's own");
  Check(!SplitTextLabel("[0000abcd]_002", strg, index), "ids are upper case");
}

void TestConvert() {
  std::u16string out;
  Check(!ConvertText(u"Same  words\nhere.", u"&just=center;Same words here.", out),
        "the same words in another layout stay the disc's");
  Check(ConvertText(u"New words", u"&just=center;&main-color=#FF0000FF;Old&main-color=#FFFFFFFF; words", out) &&
            out == u"&just=center;New words",
        "the layout prefix is kept, a leading colour is not");
  Check(ConvertText(u"A " + kRed + u"red" + kReset + u" word", u"A word", out) &&
            out == u"A &push;&main-color=#FF0A0AFF;red&pop; word",
        "a colour run is pushed and popped");
  Check(ConvertText(u"Ends " + kRed + u"red", u"x", out) && out == u"Ends &push;&main-color=#FF0A0AFF;red&pop;",
        "an open colour is closed");
  Check(ConvertText(Tag(0, 2, {char16_t(60)}) + u"Small" + Tag(0, 2, {char16_t(100)}), u"x", out) && out == u"Small",
        "a size is dropped");
  const std::u16string closing = {char16_t(0x0F), char16_t(1), char16_t(3)};
  Check(ConvertText(Tag(1, 3, {char16_t(1)}) + u"Boxed" + closing, u"x", out) && out == u"Boxed",
        "a layout tag and its closing tag are dropped");
  Check(!ConvertText(u"Press " + Tag(1, 0, {char16_t(0xCD30)}) + u" now", u"x", out), "a button keeps the disc's text");
  Check(!ConvertText(u"See " + Tag(1, 1, u"\x05TXTR_") + u" icon", u"x", out), "an icon keeps the disc's text");
  Check(!ConvertText(u"This & that", u"x", out), "an ampersand would start a tag");
  Check(!ConvertText(u"Café", u"x", out), "a glyph outside ASCII keeps the disc's text");
  const std::u16string cut = {u'a', char16_t(0x0E), char16_t(0), char16_t(3), char16_t(4), char16_t(0)};
  Check(!ConvertText(cut, u"x", out), "a tag cut short is refused");
  Check(ConvertText(u"Morphology: Stone\ncreeper\nVines grow on a mass-\nproduced rock.",
                    u"Species: Stone creeper\nVines grow on rocks.", out) &&
            out == u"Morphology: Stone creeper\nVines grow on a mass-produced rock.",
        "only the disc's own line breaks are kept");
  Check(!ConvertText(u"Abnormal heat\ntraces.", u"Abnormal heat traces.", out),
        "a string whose breaks alone changed stays the disc's");
  Check(ConvertText(u"Log 1\n\nNew text.", u"&font=1;Log 1\n\nOld text.", out) && out == u"&font=1;Log 1\n\nNew text.",
        "a blank line after a heading is kept");
}

void TestMerge() {
  const std::vector<uint8_t> retail =
      Strg({{0x4652454E, {u"un", u"deux", u"trois"}}, {0x454E474C, {u"one", u"&just=center;two", u"three"}}});
  std::map<uint32_t, std::u16string> strings;
  strings[0] = u"one";      // unchanged
  strings[1] = u"second";   // changed
  strings[7] = u"nothing";  // past the table
  std::vector<uint8_t> merged;
  int changed = 0;
  Check(MergeStringTable(retail.data(), retail.size(), strings, merged, changed) && changed == 1,
        "one string changes");
  Check(merged.size() % 32 == 0, "the table is padded to a block");
  const std::vector<uint8_t> expect =
      Strg({{0x4652454E, {u"un", u"deux", u"trois"}}, {0x454E474C, {u"one", u"&just=center;second", u"three"}}});
  Check(merged.size() >= expect.size() && std::memcmp(merged.data(), expect.data(), expect.size()) == 0,
        "the other strings and languages are as they were");

  strings.erase(1);
  Check(!MergeStringTable(retail.data(), retail.size(), strings, merged, changed) && changed == 0,
        "a table with nothing new is not written");
  strings[1] = u"second";
  for (size_t cut = 0; cut < retail.size(); ++cut) {
    Check(!MergeStringTable(retail.data(), cut, strings, merged, changed), "a table cut short is refused");
  }
}
}  // namespace

int main() {
  TestMsbt();
  TestConvert();
  TestMerge();
  if (sFailures == 0) {
    std::printf("port_remastered_text_tests: all passed\n");
  }
  return sFailures == 0 ? 0 : 1;
}
