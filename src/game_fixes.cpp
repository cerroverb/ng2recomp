// ng2 - Ninja Gaiden II static recompilation
//
// Fixes for the game's own bugs that the console hid.
//
// Stale object scripts (#94): an object's setup and cleanup run lists of
// commands through seven interpreters (sub_8248FD20 ... sub_8248FFC0, one
// per stage), each command's handler looked up in a table at 0x83AE2CA0
// (49 entries of 28 bytes, a handler per interpreter; sub_82490038 fills the
// empty ones with a default that skips the command) until the 0x7FFF that
// ends the list. When a slot is reused (sub_8249EFC8), the list of the
// object that had it last is run first, from that object's data. Going back
// to a room while things from the last one are still alive (Chapter 6's IS
// Ninjas throwing incendiary shurikens), that data may already be overwritten
// by the room's own: the room's data pack comes in at once here, where the
// console's disc took a few seconds (reads slowed to disc speed hide it), so
// the list was garbage, a command past the table's end, whose "handler" was
// 0, and the game called address 0. A command past the table, one whose
// handler isn't a function of the game, or a list that isn't readable memory
// now ends the list (logged once): the old object's leftovers aren't run,
// which is all a garbage list could have done.

#include <atomic>
#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/xmemory.h>

#include "gpu/guest.h"

namespace {

using ng2::guest::Load32;

constexpr uint32_t kCommandTable = 0x83AE2CA0;  // + command * 28: 7 handlers
constexpr uint32_t kCommandStride = 28;
constexpr uint32_t kCommandCount = 49;  // the table's entries (reverb preset names follow)
constexpr uint32_t kEndOfList = 0x7FFF;

bool Readable(uint32_t address) {
  auto* memory = rex::Runtime::instance()->memory();
  uint32_t protect = 0;
  auto* heap = memory->LookupHeap(address);
  return heap && heap->QueryProtect(address, &protect) &&
         (protect & rex::memory::kMemoryProtectRead);
}


// The game function at a handler's address (null: none), remembered: the
// lists run for every object each frame.
PPCFunc* Function(uint32_t address) {
  if (!address) {
    return nullptr;
  }
  struct Entry {
    uint32_t address = 0;
    PPCFunc* function = nullptr;
  };
  thread_local Entry cache[64];
  Entry& entry = cache[(address >> 2) & 63];
  if (entry.address != address || !entry.function) {
    PPCFunc* function = rex::Runtime::instance()->function_dispatcher()->GetFunction(address);
    if (!function) {
      return nullptr;
    }
    entry = {address, function};
  }
  return entry.function;
}

// One of the seven interpreters (sub_8248FD20 ... sub_8248FFC0): the same
// loop, each with its own handler in the table's entries (+0 ... +24), its
// call returning to `after`.
void RunCommands(PPCContext& ctx, uint8_t* base, uint32_t handlerOffset, uint32_t after) {
  const uint32_t object = ctx.r3.u32;
  const uint64_t returnAddress = ctx.lr;
  uint32_t command = ctx.r4.u32;
  uint32_t checkedPage = ~0u;
  while (true) {
    // A stale list may not be memory at all.
    if (command & 3) {
      break;
    }
    if (command >> 12 != checkedPage) {
      if (!Readable(command)) {
        break;
      }
      checkedPage = command >> 12;
    }
    const uint32_t op = Load32(command);
    if (op == kEndOfList) {
      break;
    }
    const uint32_t handler =
        op < kCommandCount ? Load32(kCommandTable + op * kCommandStride + handlerOffset) : 0;
    PPCFunc* function = Function(handler);
    if (!function) {
      static std::atomic<bool> logged{false};
      if (!logged.exchange(true)) {
        REXLOG_WARN("game fix: an object's command list at {:08X} has command {:X} with no handler "
                    "(left over from a room that was replaced, #94): the list ends there",
                    command, op);
      }
      break;
    }
    ctx.r3.u64 = object;
    ctx.r4.u64 = command;
    ctx.lr = after;
    function(ctx, base);
    command = ctx.r3.u32;  // each handler returns the next command
  }
  ctx.r3.u64 = command;
  ctx.lr = returnAddress;
}

}  // namespace

REX_HOOK_RAW(sub_8248FD20) { RunCommands(ctx, base, 0, 0x8248FD68); }
REX_HOOK_RAW(sub_8248FD90) { RunCommands(ctx, base, 4, 0x8248FDDC); }
REX_HOOK_RAW(sub_8248FE00) { RunCommands(ctx, base, 8, 0x8248FE4C); }
REX_HOOK_RAW(sub_8248FE70) { RunCommands(ctx, base, 12, 0x8248FEBC); }
REX_HOOK_RAW(sub_8248FEE0) { RunCommands(ctx, base, 16, 0x8248FF2C); }
REX_HOOK_RAW(sub_8248FF50) { RunCommands(ctx, base, 20, 0x8248FF9C); }
REX_HOOK_RAW(sub_8248FFC0) { RunCommands(ctx, base, 24, 0x8249000C); }

// Objects faded out for good (#61, Chapter 6's church window): objects between
// the camera and Ryu are faded out and back in (sub_823182A8 each frame, the
// test sub_823D1BB0: the camera against the object's collision shapes). For a
// shape it has no case for (the window's box, 0x91000004), the test goes on
// with locals only a capsule sets (the path flags at +273/+274 of its 848-byte
// frame and the capsule's ends at +240/+256), read unwritten from the stack.
// What the console left there passed; here it was a stack address, the
// distance came out NaN and counted as the camera inside, so the window stayed
// faded out (until a main weapon change left other values there). The frame is
// cleared first, with the bone at +273 set to none (255; 0 would place the
// shape at bone 0, or read a matrix at address 0 for an object without
// bones): unset, the shape is a point at the object's origin, faded only with
// the camera on it. The save area (the top 208 bytes) is left alone.
REX_EXTERN(__imp__sub_823D1BB0);
REX_HOOK_RAW(sub_823D1BB0) {
  uint8_t* frame = ng2::guest::Virtual(ctx.r1.u32 - 848);
  std::memset(frame, 0, 848 - 208);
  frame[273] = 0xFF;
  __imp__sub_823D1BB0(ctx, base);
}
