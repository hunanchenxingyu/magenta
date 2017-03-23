// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#pragma once

#include <assert.h>
#include <kernel/mutex.h>
#include <kernel/vm.h>
#include <kernel/vm/vm_page_list.h>
#include <lib/user_copy/user_ptr.h>
#include <list.h>
#include <magenta/thread_annotations.h>
#include <mxtl/array.h>
#include <mxtl/intrusive_double_list.h>
#include <mxtl/macros.h>
#include <mxtl/ref_counted.h>
#include <mxtl/ref_ptr.h>
#include <stdint.h>

class VmMapping;

typedef status_t (*vmo_lookup_fn_t)(void* context, size_t offset, size_t index, paddr_t pa);

// The base vm object that holds a range of bytes of data
//
// Can be created without mapping and used as a container of data, or mappable
// into an address space via VmAspace::MapObject
class VmObject : public mxtl::RefCounted<VmObject> {
public:
    // public API
    virtual status_t Resize(uint64_t size) { return ERR_NOT_SUPPORTED; }

    virtual uint64_t size() const { return 0; }

    // Returns the number of physical pages currently allocated to the
    // object where (offset <= page_offset < offset+len).
    // |offset| and |len| are in bytes.
    virtual size_t AllocatedPagesInRange(uint64_t offset, uint64_t len) const {
        return 0;
    }
    // Returns the number of physical pages currently allocated to the object.
    size_t AllocatedPages() const {
        return AllocatedPagesInRange(0, size());
    }

    // find physical pages to back the range of the object
    virtual status_t CommitRange(uint64_t offset, uint64_t len, uint64_t* committed) {
        return ERR_NOT_SUPPORTED;
    }

    // find a contiguous run of physical pages to back the range of the object
    virtual status_t CommitRangeContiguous(uint64_t offset, uint64_t len, uint64_t* committed,
                                           uint8_t alignment_log2) {
        return ERR_NOT_SUPPORTED;
    }

    // free a range of the vmo back to the default state
    virtual status_t DecommitRange(uint64_t offset, uint64_t len, uint64_t* decommitted) {
        return ERR_NOT_SUPPORTED;
    }

    // read/write operators against kernel pointers only
    virtual status_t Read(void* ptr, uint64_t offset, size_t len, size_t* bytes_read) {
        return ERR_NOT_SUPPORTED;
    }
    virtual status_t Write(const void* ptr, uint64_t offset, size_t len, size_t* bytes_written) {
        return ERR_NOT_SUPPORTED;
    }

    // execute lookup_fn on a given range of physical addresses within the vmo
    virtual status_t Lookup(uint64_t offset, uint64_t len, uint pf_flags,
                            vmo_lookup_fn_t lookup_fn, void* context) {
        return ERR_NOT_SUPPORTED;
    }

    // read/write operators against user space pointers only
    virtual status_t ReadUser(user_ptr<void> ptr, uint64_t offset, size_t len, size_t* bytes_read) {
        return ERR_NOT_SUPPORTED;
    }
    virtual status_t WriteUser(user_ptr<const void> ptr, uint64_t offset, size_t len,
                               size_t* bytes_written) {
        return ERR_NOT_SUPPORTED;
    }

    // translate a range of the vmo to physical addresses and store in the buffer
    virtual status_t LookupUser(uint64_t offset, uint64_t len, user_ptr<paddr_t> buffer,
                                size_t buffer_size) {
        return ERR_NOT_SUPPORTED;
    }

    virtual void Dump(uint depth, bool verbose) = 0;

    // cache maintainence operations.
    virtual status_t InvalidateCache(const uint64_t offset, const uint64_t len) {
        return ERR_NOT_SUPPORTED;
    }
    virtual status_t CleanCache(const uint64_t offset, const uint64_t len) {
        return ERR_NOT_SUPPORTED;
    }
    virtual status_t CleanInvalidateCache(const uint64_t offset, const uint64_t len) {
        return ERR_NOT_SUPPORTED;
    }
    virtual status_t SyncCache(const uint64_t offset, const uint64_t len) {
        return ERR_NOT_SUPPORTED;
    }

    virtual status_t GetMappingCachePolicy(uint32_t* cache_policy) {
        return ERR_NOT_SUPPORTED;
    }

    virtual status_t SetMappingCachePolicy(const uint32_t cache_policy) {
        return ERR_NOT_SUPPORTED;
    }

protected:
    // private constructor (use Create())
    VmObject();

    // private destructor, only called from refptr
    virtual ~VmObject();
    friend mxtl::RefPtr<VmObject>;

    DISALLOW_COPY_ASSIGN_AND_MOVE(VmObject);

    // private apis used by the VmMapping class
    // get a pointer to a page at a given offset
    friend class VmMapping;

    // get the physical address of a page at offset
    virtual status_t GetPageLocked(uint64_t offset, uint pf_flags, vm_page_t** page, paddr_t* pa) TA_REQ(lock_) {
        return ERR_NOT_SUPPORTED;
    }

    Mutex* lock() TA_RET_CAP(lock_) { return &lock_; }

    void AddMappingLocked(VmMapping* r) TA_REQ(lock_);
    void RemoveMappingLocked(VmMapping* r) TA_REQ(lock_);

    // magic value
    static const uint32_t MAGIC = 0x564d4f5f; // VMO_
    uint32_t magic_ = MAGIC;

    // members
    mutable Mutex lock_;
    mxtl::DoublyLinkedList<VmMapping*> mapping_list_ TA_GUARDED(lock_);
};

// the main VM object type, holding a list of pages
class VmObjectPaged final : public VmObject {
public:
    static mxtl::RefPtr<VmObject> Create(uint32_t pmm_alloc_flags, uint64_t size);

    static mxtl::RefPtr<VmObject> CreateFromROData(const void* data, size_t size);

    status_t Resize(uint64_t size) override;

    uint64_t size() const override { return size_; }
    size_t AllocatedPagesInRange(uint64_t offset, uint64_t len) const override;

    status_t CommitRange(uint64_t offset, uint64_t len, uint64_t* committed) override;
    status_t CommitRangeContiguous(uint64_t offset, uint64_t len, uint64_t* committed,
                                           uint8_t alignment_log2) override;
    status_t DecommitRange(uint64_t offset, uint64_t len, uint64_t* decommitted) override;

    status_t Read(void* ptr, uint64_t offset, size_t len, size_t* bytes_read) override;
    status_t Write(const void* ptr, uint64_t offset, size_t len, size_t* bytes_written) override;
    status_t Lookup(uint64_t offset, uint64_t len, uint pf_flags,
                    vmo_lookup_fn_t lookup_fn, void* context) override;

    status_t ReadUser(user_ptr<void> ptr, uint64_t offset, size_t len,
                              size_t* bytes_read) override;
    status_t WriteUser(user_ptr<const void> ptr, uint64_t offset, size_t len,
                               size_t* bytes_written) override;

    status_t LookupUser(uint64_t offset, uint64_t len, user_ptr<paddr_t> buffer,
                        size_t buffer_size) override;

    void Dump(uint depth, bool verbose) override;

    status_t InvalidateCache(const uint64_t offset, const uint64_t len) override;
    status_t CleanCache(const uint64_t offset, const uint64_t len) override;
    status_t CleanInvalidateCache(const uint64_t offset, const uint64_t len) override;
    status_t SyncCache(const uint64_t offset, const uint64_t len) override;

    status_t GetPageLocked(uint64_t offset, uint pf_flags, vm_page_t**, paddr_t*) override TA_REQ(lock_);

private:
    // private constructor (use Create())
    explicit VmObjectPaged(uint32_t pmm_alloc_flags);

    // private destructor, only called from refptr
    ~VmObjectPaged() override;

    DISALLOW_COPY_ASSIGN_AND_MOVE(VmObjectPaged);

    // perform a cache maintenance operation against the vmo.
    enum class CacheOpType { Invalidate, Clean, CleanInvalidate, Sync };
    status_t CacheOp(const uint64_t offset, const uint64_t len, const CacheOpType type);

    // add a page to the object
    status_t AddPage(vm_page_t* p, uint64_t offset);

    // internal page list routine
    void AddPageToArray(size_t index, vm_page_t* p);

    // internal read/write routine that takes a templated copy function to help share some code
    template <typename T>
    status_t ReadWriteInternal(uint64_t offset, size_t len, size_t* bytes_copied, bool write,
                               T copyfunc);

// constants
#if _LP64
    static const uint64_t MAX_SIZE = ROUNDDOWN(SIZE_MAX, PAGE_SIZE);
#else
    static const uint64_t MAX_SIZE = SIZE_MAX * PAGE_SIZE;
#endif

    // members
    uint64_t size_ = 0;
    uint32_t pmm_alloc_flags_ = PMM_ALLOC_FLAG_ANY;

    // a tree of pages
    VmPageList page_list_ TA_GUARDED(lock_);
};

// VMO representing a physical range of memory
class VmObjectPhysical final : public VmObject {
public:
    static mxtl::RefPtr<VmObject> Create(paddr_t base, uint64_t size);

    status_t LookupUser(uint64_t offset, uint64_t len, user_ptr<paddr_t> buffer,
                        size_t buffer_size) override;

    void Dump(uint depth, bool verbose) override;

    status_t GetPageLocked(uint64_t offset, uint pf_flags, vm_page_t**, paddr_t* pa) override TA_REQ(lock_);

    status_t GetMappingCachePolicy(uint32_t* cache_policy) override;
    status_t SetMappingCachePolicy(uint32_t cache_policy) override;

private:
    // private constructor (use Create())
    VmObjectPhysical(paddr_t base, uint64_t size);

    // private destructor, only called from refptr
    ~VmObjectPhysical() override;

    DISALLOW_COPY_ASSIGN_AND_MOVE(VmObjectPhysical);

    // members
    const uint64_t size_ = 0;
    const paddr_t base_ = 0;
    uint32_t mapping_cache_flags_ = 0;
    bool mapping_cache_flags_set_ = false;
};
