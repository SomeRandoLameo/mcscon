// Tiny boundary-tag allocator over caller memory (8-byte aligned blocks, first fit, coalescing on free).
// Offsets (u32) are used instead of pointers; 0 means "null". No dependencies, no heap.
#pragma once
#include "types.h"
#include <string.h>

namespace mc {
class Arena {
public:
    Arena() : base_(0), size_(0), used_(0) {}
    // `mem` must be 8-byte aligned.
    void init(void* mem, size_t size) {
        base_ = (u8*)mem; size_ = (u32)(size & ~(size_t)7); used_ = 0;
        if (size_ < 32) { size_ = 0; return; }
        setHdr(0, size_ - 8, false, 0);          // one big free block, then an end sentinel header
        setHdr(size_ - 8, 0, true, size_ - 8);
    }
    u32 alloc(u32 bytes) {
        u32 need = ((bytes + 7) & ~7u) + 8;
        for (u32 o = 0; size_ && sizeOf(o) != 0; o += sizeOf(o)) {
            if (isUsed(o) || sizeOf(o) < need) continue;
            u32 sz = sizeOf(o);
            if (sz - need >= 16) {               // split
                setHdr(o, need, true, prevOf(o));
                setHdr(o + need, sz - need, false, need);
                setPrev(o + need + (sz - need), sz - need);
            } else { setHdr(o, sz, true, prevOf(o)); }
            used_ += sizeOf(o);
            return o + 8;
        }
        return 0;
    }
    void release(u32 off) {
        if (!off) return;
        u32 o = off - 8, sz = sizeOf(o); used_ -= sz;
        u32 nx = o + sz;
        if (sizeOf(nx) && !isUsed(nx)) { sz += sizeOf(nx); }
        u32 pv = prevOf(o);
        if (o && pv && !isUsed(o - pv)) { o -= pv; sz += pv; }
        setHdr(o, sz, false, o ? prevOf(o) : 0);
        setPrev(o + sz, sz);
    }
    u8* ptr(u32 off) const { return base_ + off; }
    u32 capacity() const { return size_; }
    u32 used() const { return used_; }
    // size of the largest free block (fragmentation indicator)
    u32 largestFree() const { u32 m = 0; for (u32 o = 0; size_ && sizeOf(o) != 0; o += sizeOf(o)) if (!isUsed(o) && sizeOf(o) > m) m = sizeOf(o); return m > 8 ? m - 8 : 0; }
private:
    u32* h(u32 o) const { return (u32*)(base_ + o); }
    u32 sizeOf(u32 o) const { return h(o)[0] & ~1u; }
    bool isUsed(u32 o) const { return h(o)[0] & 1u; }
    u32 prevOf(u32 o) const { return h(o)[1]; }
    void setHdr(u32 o, u32 sz, bool used, u32 prev) { h(o)[0] = sz | (used ? 1u : 0u); h(o)[1] = prev; }
    void setPrev(u32 o, u32 prev) { h(o)[1] = prev; }
    u8* base_; u32 size_, used_;
};
} // namespace mc
