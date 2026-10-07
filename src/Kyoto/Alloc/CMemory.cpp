#include "Kyoto/Alloc/CMemory.hpp"
#include "Kyoto/Alloc/CMemorySys.hpp"

#include "Kyoto/Alloc/CCallStack.hpp"
#include "Kyoto/Alloc/CGameAllocator.hpp"
#include "Kyoto/Basics/RAssertDolphin.hpp"

#include "dolphin/os.h"

#ifdef TARGET_PC
#include <mutex>

// The guest serialised the heap with OSDisableInterrupts, which is a no-op on PC,
// and the heap is not only touched by the game thread here: Aurora's DVD worker
// (CDSPStream::ReadCompleted) and the MusyX mixer thread (CDSPStream::UpdateStream)
// free one-shot stream buffers. Recursive in case an out-of-memory callback
// allocates or frees again from inside the allocator. Never destroyed, so a free
// from a static destructor after main still finds it.
static std::recursive_mutex& HeapMutex() {
  static std::recursive_mutex* mutex = new std::recursive_mutex;
  return *mutex;
}
#endif

static CGameAllocator gGameAllocator;
IAllocator* CMemory::mpAllocator = &gGameAllocator;
bool CMemory::mInitialized;
uint gLeakCount = 0;
uint gLeakBytes = 0;

CMemorySys::CMemorySys(COsContext& ctx, IAllocator& allocator) {
  CMemory::Startup(ctx);
  CMemory::SetAllocator(ctx, allocator);
}

CMemorySys::~CMemorySys() { CMemory::Shutdown(); }

IAllocator& CMemorySys::GetGameAllocator() { return gGameAllocator; }

void CMemory::Startup(COsContext& ctx) { mInitialized = mpAllocator->Initialize(ctx); }

void CMemory::SetAllocator(COsContext& ctx, IAllocator& allocator) {
  if (mpAllocator != &allocator) {
    if (mpAllocator != nullptr) {
      mpAllocator->ReleaseAll();
    }
    mpAllocator = &allocator;
    mpAllocator->Initialize(ctx);
  }
}

static bool cmemory_enum_alloc_cb(const IAllocator::SAllocInfo& info, const void* ptr) {
  if (info.x8_isAllocated && info.x9_ == 0) {
    ++gLeakCount;
    gLeakBytes += info.x4_len;
  }
  return true;
}

void CMemory::Shutdown() {
  CMemory::mInitialized = false;

  if (mpAllocator->GetMetrics().x8_ != 0) {
    gLeakCount = 0;
    gLeakBytes = 0;
    mpAllocator->EnumAllocations((IAllocator::FEnumAllocationsCb)cmemory_enum_alloc_cb, nullptr,
                                 false);
  }
  mpAllocator->Shutdown();
}

void* CMemory::Alloc(size_t len, IAllocator::EHint hint, IAllocator::EScope scope,
                     IAllocator::EType type, const CCallStack& callstack) {
#ifdef TARGET_PC
  std::lock_guard< std::recursive_mutex > heapLock(HeapMutex());
#endif
  volatile bool enabled = OSDisableInterrupts();
  void* ret = mpAllocator->Alloc(len, hint, scope, type, callstack);
  if (ret == nullptr) {
    rs_debugger_printf("Alloc failed - Size: %zu", len);
#ifdef TARGET_PC
    OSRestoreInterrupts(enabled);
    throw std::bad_alloc();
#endif
  }

  OSRestoreInterrupts(enabled);
  return ret;
}

void CMemory::Free(const void* ptr) {
#ifdef TARGET_PC
  std::lock_guard< std::recursive_mutex > heapLock(HeapMutex());
#endif
  volatile bool enabled = OSDisableInterrupts();
  if (ptr != nullptr) {
    mpAllocator->Free(ptr);
  }
  OSRestoreInterrupts(enabled);
}

#ifdef TARGET_PC
// Port: MP_HEAP_CHECK hooks for CFrameDelayedKiller (see CGameAllocator::CheckHeap).
bool PortHeapCheckEnabled() { return CGameAllocator::HeapCheckEnabled(); }

void PortHeapCheckAll() {
  std::lock_guard< std::recursive_mutex > heapLock(HeapMutex());
  gGameAllocator.CheckHeap();
}

bool PortHeapPeekBlock(const void* ptr, const char** fileAndLine, size_t* len) {
  std::lock_guard< std::recursive_mutex > heapLock(HeapMutex());
  return gGameAllocator.PeekLiveBlock(ptr, fileAndLine, len);
}
#endif

void CMemory::SetOutOfMemoryCallback(IAllocator::FOutOfMemoryCb cb, const void* context) {
  mpAllocator->SetOutOfMemoryCallback(cb, context);
}

void CMemory::OffsetFakeStatics(int offset) { mpAllocator->OffsetFakeStatics(offset); }

void* operator new(size_t sz, const char* fileAndLine, const char* type) {
  return CMemory::Alloc(sz, IAllocator::kHI_None, IAllocator::kSC_Unk1, IAllocator::kTP_Heap,
                        CCallStack(-1, fileAndLine, type));
}
void* operator new[](size_t sz, const char* fileAndLine, const char* type) {
  return CMemory::Alloc(sz, IAllocator::kHI_None, IAllocator::kSC_Unk1, IAllocator::kTP_Array,
                        CCallStack(-1, fileAndLine, type));
}
