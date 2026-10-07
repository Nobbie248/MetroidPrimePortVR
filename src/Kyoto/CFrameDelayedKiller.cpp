#include "Kyoto/CFrameDelayedKiller.hpp"

#include "Kyoto/Particles/IElement.hpp"

#include <dolphin/gx/GXManage.h>
#include <rstl/list.hpp>

#if NONMATCHING || defined(TARGET_PC)
#include <stdint.h>
#endif
#ifdef TARGET_PC
#include <stdlib.h>
#include <vector>

#include "port_log.h"
#endif

static uint sCurList = 0;
static rstl::list< void* > sFrameDelayedList[2];
#ifdef TARGET_PC
static rstl::list< void* > sHostFrameDelayedList[2];

// MP_HEAP_CHECK=1: remember every queued victim (and, for game-heap blocks, what its header said
// at that moment) so a pointer queued twice, or one whose block was freed and reused before the
// flush, is reported instead of corrupting the heap. Hooks live in Alloc/CMemory.cpp.
bool PortHeapCheckEnabled();
void PortHeapCheckAll();
bool PortHeapPeekBlock(const void* ptr, const char** fileAndLine, size_t* len);

namespace {
struct SQueuedVictim {
  void* x0_ptr;
  const char* x8_fileAndLine;
  size_t x10_len;
  bool x18_hasHeader;
  bool x19_host;
};
std::vector< SQueuedVictim > sQueuedVictims[2];

[[noreturn]] void ReportQueueProblem(const char* what, const SQueuedVictim& v, uint list) {
  PortLog::Write("HEAP CHECK: %s: ptr=%p host=%d list=%u cur=%u header-at-queue: file=\"%s\" "
                 "len=0x%zx\n",
                 what, v.x0_ptr, v.x19_host ? 1 : 0, list, sCurList,
                 v.x18_hasHeader && v.x8_fileAndLine != nullptr ? v.x8_fileAndLine : "<none>",
                 v.x10_len);
  fflush(stderr);
  abort();
}

void NoteQueued(uint index, void* victim, bool host) {
  SQueuedVictim v = {victim, nullptr, 0, false, host};
  for (uint list = 0; list < 2; ++list) {
    for (const SQueuedVictim& other : sQueuedVictims[list]) {
      if (other.x0_ptr == victim) {
        ReportQueueProblem("pointer queued for deletion twice", other, list);
      }
    }
  }
  if (!host) {
    v.x18_hasHeader = PortHeapPeekBlock(victim, &v.x8_fileAndLine, &v.x10_len);
  }
  sQueuedVictims[index].push_back(v);
}

// Called with the list about to be freed: every queued game-heap block must still be the
// allocation it was when queued.
void VerifyQueued(uint index) {
  PortHeapCheckAll();
  for (const SQueuedVictim& v : sQueuedVictims[index]) {
    if (!v.x18_hasHeader) {
      continue;
    }
    const char* fileAndLine = nullptr;
    size_t len = 0;
    if (!PortHeapPeekBlock(v.x0_ptr, &fileAndLine, &len)) {
      ReportQueueProblem("queued block is no longer a live allocation", v, index);
    }
    if (fileAndLine != v.x8_fileAndLine || len != v.x10_len) {
      ReportQueueProblem("queued block was freed and reused before its flush", v, index);
    }
  }
  sQueuedVictims[index].clear();
}
} // namespace
#endif

#if defined(__MWERKS__) && (VERSION < VERSION_GM8P_00 || VERSION == VERSION_GM8E_02)
#pragma force_active on
CFrameDelayedKiller::Stats CFrameDelayedKiller::mUnusedStats = {0, 0, 0, 0, 0, 0};
#pragma force_active reset
#endif

void CFrameDelayedKiller::Initialize() { StallAndFlushAllAllocations(); }

void CFrameDelayedKiller::ShutDown() { StallAndFlushAllAllocations(); }

void CFrameDelayedKiller::FlushAllAllocations() {
  for (int i = 0; i < 2; ++i) {
    FlushAllocationsForFrame();
  }
}

void CFrameDelayedKiller::StallAndFlushAllAllocations() {
  GXDrawDone();
  FlushAllAllocations();
}

void CFrameDelayedKiller::ScheduleDeletion(const EWhichFrame thisFrame, void* victim) {
  uint index = thisFrame == true ? sCurList : sCurList ^ 1;

#ifdef TARGET_PC
  if (PortHeapCheckEnabled()) {
    NoteQueued(index, victim, false);
  }
#endif
  sFrameDelayedList[index].push_back(victim);
}

#ifdef TARGET_PC
void CFrameDelayedKiller::ScheduleHostDeletion(const EWhichFrame thisFrame, void* victim) {
  uint index = thisFrame == true ? sCurList : sCurList ^ 1;
  if (PortHeapCheckEnabled()) {
    NoteQueued(index, victim, true);
  }
  sHostFrameDelayedList[index].push_back(victim);
}
#endif

void CFrameDelayedKiller::FlushAllocationsForFrame() {
  sCurList ^= 1;
#ifdef TARGET_PC
  if (PortHeapCheckEnabled()) {
    VerifyQueued(sCurList);
  }
#endif
  rstl::list< void* >& list = sFrameDelayedList[sCurList];
  for (rstl::list< void* >::iterator t = list.begin(); t != list.end(); ++t) {
    CMemory::Free(*t);
  }

  list.clear();
#ifdef TARGET_PC
  rstl::list< void* >& hostList = sHostFrameDelayedList[sCurList];
  for (rstl::list< void* >::iterator t = hostList.begin(); t != hostList.end(); ++t) {
    delete[] static_cast< uchar* >(*t);
  }
  hostList.clear();
#endif
}

CElementAllocationChunk::CElementAllocationChunk()
: x0_capacity(256)
, x4_allocatedWords(0)
, x8_allocationCount(0) {}

bool CElementAllocationChunk::CanAllocate(uint size) const {
  return x0_capacity > x4_allocatedWords + (size + 3) / 4;
}

bool CElementAllocationChunk::Contains(const void* ptr) const {
#if NONMATCHING || defined(TARGET_PC)
  return reinterpret_cast< uintptr_t >(ptr) - reinterpret_cast< uintptr_t >(xc_data) <
         sizeof(xc_data);
#else
  int offset = static_cast< const char* >(ptr) - reinterpret_cast< const char* >(xc_data);
  int index = offset / 4;
  return x0_capacity > index;
#endif
}

void* CElementAllocationChunk::Allocate(uint size) {
  void* ptr = &xc_data[x4_allocatedWords];
  x4_allocatedWords += (size + 3) / 4;
  ++x8_allocationCount;
  return ptr;
}

void CElementAllocationChunk::Free(void*) { --x8_allocationCount; }

void CElementAllocationChunk::Rewind(uint size) {
  uint words = (size + 3) / 4;
  if (words > x4_allocatedWords) {
    x4_allocatedWords = 0;
  } else {
    x4_allocatedWords -= words;
  }
}

uint CElementAllocationChunk::GetAllocatedSize() const { return x4_allocatedWords * 4; }

uint CElementAllocationChunk::GetAllocationCount() const { return x8_allocationCount; }

void* IElement::operator new(size_t sz, const char* fileAndLine, const char* type) {
  return CElementAllocator::Alloc(sz, fileAndLine, type);
}

void IElement::operator delete(void* ptr, const size_t sz) { CElementAllocator::Free(ptr, sz); }
