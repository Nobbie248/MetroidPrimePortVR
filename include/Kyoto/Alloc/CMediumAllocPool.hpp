#ifndef _CMEDIUMALLOCPOOL
#define _CMEDIUMALLOCPOOL

#include <rstl/auto_ptr.hpp>
#include <rstl/list.hpp>

struct SMediumAllocPuddle {
  SMediumAllocPuddle(const uint numBlocks, void* data, const bool canErase);
  ~SMediumAllocPuddle();
  void* FindFree(uint blockCount);
  void* FindFreeEntry(uint blockCount);
  void Free(const void* ptr);

  const uint GetNumBlocks() const { return x14_numBlocks; }
  const uint GetNumAllocs() const { return x18_numAllocs; }
  const uint GetNumEntries() const { return x1c_numEntries; }
  const bool CanErase() const { return x20_canErase; }
  // size_t, not uint: a 64-bit difference truncated to 32 bits can land
  // a pointer from elsewhere inside the puddle.
  const size_t GetPtrOffset(const void* ptr) const {
    return size_t((const uchar*)ptr - x0_mainData.get());
  }
  static ushort GetBlockOffset(const void* ptrA, const void* ptrB);
  static void InitBookKeeping(uchar* bookKeepingPtr, const ushort blockCount);

private:
  rstl::auto_ptr< rstl::game_memory< uchar > > x0_mainData;
  uchar* x8_bookKeeping;
  uchar* xc_cachedBookKeepingAddr;
  uint x10_unused;
  uint x14_numBlocks;
  uint x18_numAllocs;
  uint x1c_numEntries;
  bool x20_canErase : 1;
};

class CMediumAllocPool {
public:
  rstl::list< SMediumAllocPuddle > x0_list;
  rstl::list< SMediumAllocPuddle >::iterator x18_lastNodePrev;
  CMediumAllocPool();
  void* Alloc(uint size);
  bool HasPuddles() const;
  void AddPuddle(const uint, void*, const bool);
  void ClearPuddles();

  int Free(const void* ptr);

  uint GetTotalEntries();
  uint GetNumBlocksAvailable();
  uint GetNumAllocs();

  static uint GetAllocMemoryRequired(uint numBlocks) { return numBlocks * 32; }
  static uint GetBookKeepingMemoryRequired(uint numBlocks) { return numBlocks; }

  static CMediumAllocPool* gMediumAllocPtr;
};

#endif // _CMEDIUMALLOCPOOL
