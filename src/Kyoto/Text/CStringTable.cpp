#include "Kyoto/Text/CStringTable.hpp"

#include <Kyoto/Streams/CInputStream.hpp>
#if TARGET_LITTLE_ENDIAN || WCHAR_MAX > 0xffff
#include "Kyoto/Basics/CBasics.hpp"
#include "Kyoto/Streams/CMemoryInStream.hpp"
#include <string.h>
#endif

#ifdef TARGET_PC
#include "port_apclient.h"
#include "port_custom_res.h"
#include "port_hints.h"

#include <string>
#include <vector>
#endif

#include <rstl/pair.hpp>
#include <rstl/vector.hpp>

static FourCC mCurrentLanguage = 'ENGL';
static const wchar_t skInvalidString[] = L"Invalid";

CStringTable::CStringTable(CInputStream& in) : x0_stringCount(0), x4_data(NULL) {
  in.ReadLong();
  in.ReadLong();
  int langCount = in.Get(TType< int >());
  x0_stringCount = in.Get(TType< uint >());
  rstl::vector< rstl::pair< FourCC, uint > > langOffsets(langCount);
  for (int i = 0; i < langCount; ++i) {
    langOffsets.push_back(in.Get(TType< rstl::pair< FourCC, uint > >()));
  }

  int offset = langOffsets.front().second;
  for (int i = 0; i < langCount; ++i) {
    if (langOffsets[i].first == mCurrentLanguage) {
      offset = langOffsets[i].second;
      break;
    }
  }
  for (uint i = 0; i < offset; ++i) {
    in.ReadChar();
  }

  uint dataLen = in.Get(TType< uint >());
#if TARGET_LITTLE_ENDIAN || WCHAR_MAX > 0xffff
  rstl::vector< uchar > data(dataLen, uchar(0));
  in.ReadBytes(data.data(), dataLen);
  if (x0_stringCount < 0 || static_cast< uint >(x0_stringCount) > dataLen / sizeof(uint)) {
    x0_stringCount = 0;
    return;
  }

  CMemoryInStream offsets(data.data(), x0_stringCount * sizeof(uint));
  mNativeStrings.reserve(x0_stringCount);
  for (int i = 0; i < x0_stringCount; ++i) {
    uint pos = offsets.ReadLong();
    rstl::vector< wchar_t > text;
    bool terminated = false;
    while (pos <= dataLen && dataLen - pos >= sizeof(ushort)) {
      ushort unit;
      memcpy(&unit, data.data() + pos, sizeof(unit));
      uint codepoint = CBasics::SwapBytes(unit);
      pos += sizeof(unit);
      if (codepoint == 0) {
        terminated = true;
        break;
      }

      // Preserve UTF-16 on 16-bit wchar_t hosts; combine surrogate pairs on
      // hosts whose wide characters can hold a complete Unicode code point.
      if (sizeof(wchar_t) > sizeof(ushort) && codepoint >= 0xd800 && codepoint <= 0xdbff &&
          dataLen - pos >= sizeof(ushort)) {
        memcpy(&unit, data.data() + pos, sizeof(unit));
        const ushort low = CBasics::SwapBytes(unit);
        if (low >= 0xdc00 && low <= 0xdfff) {
          codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + low - 0xdc00;
          pos += sizeof(unit);
        }
      }
      text.push_back(static_cast< wchar_t >(codepoint));
    }
    if (!terminated) {
      text.clear();
      for (const wchar_t* c = skInvalidString; *c; ++c) {
        text.push_back(*c);
      }
    }
    text.push_back(0);
    mNativeStrings.push_back(text);
  }
#else
  x4_data = rs_new uchar[dataLen];
  in.ReadBytes(x4_data.get(), dataLen);
#endif
}

const wchar_t* CStringTable::GetString(int idx) const {
  if (idx < 0 || idx >= x0_stringCount) {
    return skInvalidString;
  }
#ifdef TARGET_PC
  if (mPortWatchedId != 0 && idx == x0_stringCount - 1) {
    // Tables are heap objects from the factory, never const-defined.
    const_cast< CStringTable* >(this)->PortRefreshWatched();
  }
#endif
#if TARGET_LITTLE_ENDIAN || WCHAR_MAX > 0xffff
  return mNativeStrings[idx].data();
#else
  int offset = *(reinterpret_cast< const int* >(x4_data.get()) + idx);
  return reinterpret_cast< const wchar_t* >(x4_data.get() + offset);
#endif
}

#ifdef TARGET_PC
void CStringTable::PortSetString(int idx, const unsigned short* text, int length) {
  if (idx < 0 || idx >= x0_stringCount) {
    return;
  }
  rstl::vector< wchar_t > native;
  for (int i = 0; i < length; ++i) {
    uint codepoint = text[i];
    // Same as the loader: whole code points where wchar_t holds them.
    if (sizeof(wchar_t) > sizeof(ushort) && codepoint >= 0xd800 && codepoint <= 0xdbff &&
        i + 1 < length && text[i + 1] >= 0xdc00 && text[i + 1] <= 0xdfff) {
      codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + text[i + 1] - 0xdc00;
      ++i;
    }
    native.push_back(static_cast< wchar_t >(codepoint));
  }
  native.push_back(0);
  mNativeStrings[idx] = native;
}

void CStringTable::PortSetCount(int count) {
  mNativeStrings.clear();
  mNativeStrings.reserve(count);
  for (int i = 0; i < count; ++i) {
    mNativeStrings.push_back(rstl::vector< wchar_t >(1, L'\0'));
  }
  x0_stringCount = count;
}

void CStringTable::PortWatch(uint strgId) {
  mPortWatchedId = strgId;
  PortRefreshWatched();
}

void CStringTable::PortRefreshWatched() {
  std::u16string text;
  // No text (yet, or after a disconnect) keeps whatever the string last said.
  if (x0_stringCount <= 0 || !PortHints::WatchedText(mPortWatchedId, text) ||
      text == mPortWatchedText) {
    return;
  }
  mPortWatchedText = text;
  PortSetString(x0_stringCount - 1, reinterpret_cast< const unsigned short* >(text.data()),
                static_cast< int >(text.size()));
}
#endif

const CFactoryFnReturn FStringTableFactory(const SObjectTag& tag, CInputStream& in,
                                     const CVParamTransfer& xfer) {
#ifdef TARGET_PC
  CStringTable* table = rs_new CStringTable(in);
  // The Artifact Temple totems say where a randomized seed put each artifact,
  // and a randomized pickup's scan what it holds.
  if (PortHints::IsWatched(tag.GetId())) {
    table->PortWatch(tag.GetId());
  }
  // An Archipelago seed's elevator texts and temple objective.
  std::vector< std::string > seedStrings;
  if (PortAp::SeedStrings(tag.GetId(), seedStrings)) {
    table->PortSetCount(static_cast< int >(seedStrings.size()));
    for (size_t i = 0; i < seedStrings.size(); ++i) {
      const std::u16string text = PortCustomRes::Utf16(seedStrings[i]);
      table->PortSetString(static_cast< int >(i),
                           reinterpret_cast< const unsigned short* >(text.data()),
                           static_cast< int >(text.size()));
    }
  }
  // The completion screen names the seed (STRG_CompletionScreen, string 1).
  std::string resultsLine;
  if (tag.GetId() == 0x95019A7A && PortAp::SeedResultsLine(resultsLine)) {
    const std::u16string text = PortCustomRes::Utf16(resultsLine + "\nPercentage Complete");
    table->PortSetString(1, reinterpret_cast< const unsigned short* >(text.data()),
                         static_cast< int >(text.size()));
  }
  return table;
#else
  return rs_new CStringTable(in);
#endif
}
