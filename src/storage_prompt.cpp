// ng2 - Ninja Gaiden II static recompilation
//
// The game's text lookup (sub_837578C8: language, id), hooked:
// - The PC has one storage device, so the game's "Change Storage Device"
//   prompts (Load Game's button line, the Options pages' hints, NINJA
//   CINEMA's) are left out, in every language: for each id the English text
//   is looked at once, and where it holds "@[pad:..] Change Storage Device"
//   (or NINJA CINEMA's "@[pad:..] Storage Device"),
//   that button's segment (from its tag to the next tag) is cut out of the
//   string handed out, where it lies (the table's own copy, so once). Untagged
//   labels of the same words are left alone.
// - RECOMP SETTINGS' texts are given for the ids it uses (recomp_settings.cpp),
//   and its description line's id is replaced.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include <rex/hook.h>
#include <rex/ppc/context.h>

#include "gpu/guest.h"

namespace ng2::recomp_settings {
uint32_t RecompText(uint32_t id);
}

namespace {

constexpr uint32_t kEnglish = 1;  // the lookup's languages: 0 Japanese, 1 English, ...
constexpr std::u16string_view kPrompt = u"Change Storage Device";
constexpr std::u16string_view kShortPrompt = u"Storage Device";  // (NINJA CINEMA's)
constexpr std::u16string_view kTag = u"@[";           // any tag
constexpr std::u16string_view kButton = u"@[pad:";    // a button's
constexpr uint32_t kMaxChars = 1024;

char16_t At(const uint8_t* p) {
  return char16_t(p[0] << 8 | p[1]);
}

bool Matches(const uint8_t* p, std::u16string_view text) {
  for (size_t i = 0; i < text.size(); i++) {
    if (At(p + 2 * i) != text[i]) {
      return false;
    }
  }
  return true;
}

// A UTF-16 BE string's length, or kMaxChars if it doesn't end in time.
uint32_t Length(const uint8_t* s) {
  uint32_t length = 0;
  while (length < kMaxChars && At(s + 2 * length) != 0) {
    length++;
  }
  return length;
}

// The button tag ("@[pad:..]") before the prompt in an English string, or
// empty if it has none.
std::u16string PromptTag(const uint8_t* s) {
  uint32_t length = Length(s);
  if (length == kMaxChars) {
    return {};
  }
  for (uint32_t tag = 0; tag + kButton.size() < length; tag++) {
    if (!Matches(s + 2 * tag, kButton)) {
      continue;
    }
    uint32_t close = tag + uint32_t(kButton.size());
    while (close < length && At(s + 2 * close) != u']') {
      close++;
    }
    uint32_t text = close + 1;
    while (text < length && At(s + 2 * text) == u' ') {
      text++;
    }
    if ((text + kPrompt.size() <= length && Matches(s + 2 * text, kPrompt)) ||
        (text + kShortPrompt.size() <= length && Matches(s + 2 * text, kShortPrompt))) {
      std::u16string found;
      for (uint32_t i = tag; i <= close && i < length; i++) {
        found += At(s + 2 * i);
      }
      return found;
    }
  }
  return {};
}

// That button's segment cut out of a string at a guest address: from its
// tag to the next tag, a line break or the end.
void CutSegment(uint32_t address, std::u16string_view tag) {
  // (heap only: the image's read-only data can't be written)
  if (address < 0x40000000u || address >= 0xF0000000u ||
      (address >= 0x82000000u && address < 0x90000000u)) {
    return;
  }
  uint8_t* s = ng2::guest::Virtual(address);
  uint32_t length = Length(s);
  if (length == kMaxChars) {
    return;
  }
  for (uint32_t at = 0; at + tag.size() <= length; at++) {
    if (!Matches(s + 2 * at, tag)) {
      continue;
    }
    uint32_t end = at + uint32_t(tag.size());
    while (end < length && At(s + 2 * end) != u'\n' &&
           !(end + kTag.size() <= length && Matches(s + 2 * end, kTag))) {
      end++;
    }
    if (end == length || At(s + 2 * end) == u'\n') {
      // (the last segment: the spaces before it go instead)
      while (at > 0 && At(s + 2 * (at - 1)) == u' ') {
        at--;
      }
    }
    std::memmove(s + 2 * at, s + 2 * end, 2 * (length - end));
    std::memset(s + 2 * (at + length - end), 0, 2 * (end - at));
    return;
  }
}

// Per id: the prompt's button tag in its English text ("": none), found once.
std::mutex g_tagsMutex;
std::unordered_map<uint32_t, std::u16string> g_tags;

}  // namespace

// RECOMP SETTINGS' description line (recomp_settings.cpp): the text id the
// hints' selector asks for instead of its page's (its two description
// lookups, by return address); 0: the game's. (The game's thread only.)
std::atomic<uint32_t> g_descriptionText{0};
void SetDescriptionText(uint32_t id) {
  g_descriptionText.store(id, std::memory_order_relaxed);
}

REX_EXTERN(__imp__sub_837578C8);
REX_HOOK_RAW(sub_837578C8) {
  uint32_t from = uint32_t(ctx.lr);
  if (from == 0x836D6B68 || from == 0x836D6C24) {
    if (uint32_t id = g_descriptionText.load(std::memory_order_relaxed)) {
      ctx.r4.u64 = id;
    }
  }
  uint32_t id = ctx.r4.u32;
  // RECOMP SETTINGS' texts, in every language, without the game's lookup:
  // some of their ids are past the end of the game's table.
  if (uint32_t ours = ng2::recomp_settings::RecompText(id)) {
    ctx.r3.u64 = ours;
    return;
  }
  // (an id of ours past the table not given a text: the table's empty one,
  // not a read past its end)
  if (id >= 0x20300 && id < 0x20380) {
    ctx.r4.u64 = id = 0x20034;
  }
  // The English text first (once per id, once it's loaded), for the prompt's
  // tag. (Map nodes stay put: the pointer outlives the lock.)
  const std::u16string* tag = nullptr;
  {
    std::lock_guard lock(g_tagsMutex);
    if (auto it = g_tags.find(id); it != g_tags.end()) {
      tag = &it->second;
    }
  }
  if (!tag) {
    PPCContext saved = ctx;
    ctx.r1.u64 = saved.r1.u32 - 0x200;
    ctx.r3.u64 = kEnglish;
    ctx.r4.u64 = id;
    __imp__sub_837578C8(ctx, base);
    uint32_t english = ctx.r3.u32;
    ctx = saved;
    if (english) {
      std::lock_guard lock(g_tagsMutex);
      tag = &g_tags.try_emplace(id, PromptTag(ng2::guest::Virtual(english))).first->second;
    }
  }
  __imp__sub_837578C8(ctx, base);
  if (tag && !tag->empty() && ctx.r3.u32) {
    CutSegment(ctx.r3.u32, *tag);
  }
}
