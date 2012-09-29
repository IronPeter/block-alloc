//
//  main.cpp
//  fmallo
//
//  Created by Петр on 21.09.12.
//  Copyright (c) 2012 Петр. All rights reserved.
//
#include <sys/mman.h>
#include <pthread.h>
#include <vector>
#include <stdlib.h>
#include <stdio.h>
#include <algorithm>
#include <memory.h>


#define CHUNK_ORDER 8
#define PAGE_SIZE (4096)
#define CHUNK_SIZE (PAGE_SIZE * CHUNK_ORDER)
#define SINGLE_ALLOC (PAGE_SIZE)
#define ORDERS 1024
#define DUMP_STAT false

size_t Align(size_t value, size_t align) {
    return (value + align - 1) & ~(align - 1);
}

static const bool enablestat = DUMP_STAT;
static long long miss = 0;
static long long memocount = 0;
static long long allocount = 0;
static long long blckcount = 0;
static long long counts[ORDERS] = {0};
static volatile long locks[ORDERS] = {0};
static void *list[ORDERS] = {0};
static volatile long counter = 0;
static pthread_key_t allocatorKey;
static volatile long init = 0;

void Destructor(void *data);


void *Advance(void *block, size_t size) {
    return (void *)((char *)block + size);
}

size_t PopPages(size_t order, void **pages, size_t num) {
    if (order >= ORDERS || list[order] == 0) {
        return 0;
    }
    while (!__sync_bool_compare_and_swap (&locks[order], 0, 1));
    void *res = list[order];
    size_t count = 0;
    while (res && count < num) {
      pages[count] = res;
      ++count;
      res = *(void **)res;
    }
    list[order] = res;
    asm volatile ("" : : : "memory");
    locks[order] = 0;
    asm volatile ("" : : : "memory");
    return count;
};

bool PushPage(void *page, size_t order) {
    if (order >= ORDERS) {
        return false;
    }
    while (!__sync_bool_compare_and_swap (&locks[order], 0, 1));
    void *res = list[order];
    *(void **)page = res;
    list[order] = page;
    asm volatile ("" : : : "memory");
    locks[order] = 0;
    asm volatile ("" : : : "memory");
    return true;
}

void DumpAllocStats() {
    if (!enablestat) {
        return;
    }
    printf("%lld %lld %lld %f\n", memocount, allocount, blckcount, double(miss));
    for (size_t i = 0; i < ORDERS; ++i) {
        if (counts[i]) {
            int size = (int)((PAGE_SIZE * i) / 1024);
            printf(" %dK:%lld:%lld\n", size, counts[i], counts[i] * size);
        }
    }
    printf("\n");
};

size_t LowLevelAlloc(size_t order, void **pages, size_t num){
    size_t size = order * num;
    void *map = mmap(0, size * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    for (size_t i = 0; i < num; ++i) {
        pages[i] = map;
        map = Advance(map, order * PAGE_SIZE);
    }
    if (enablestat) {
        __sync_add_and_fetch(&blckcount, num * order * PAGE_SIZE);
        __sync_add_and_fetch(&counts[std::min((size_t)ORDERS - 1, order)], num);
    }
    return num;
}

void *SysAlloc(size_t &size) {
    size = Align(size, PAGE_SIZE);
    size_t order = size / PAGE_SIZE;
    void *data[1] = {0};
    size_t num = PopPages(order, data, 1);
    if (num) {
        return data[0];
    }
    LowLevelAlloc(order, data, 1);
    return data[0];
}

struct TAllocThreadState {
    void *Pages[8];
    size_t PageNum;
    void *Chunk;
    size_t Ptr;
    void *Block;
    size_t AllocLimit;
    int Counter;
};

struct TBlockHeader {
    size_t Size;
    int RefCount;
    int AllCount;
};

void UnMap(void *block, size_t order) {
    size_t size = order * PAGE_SIZE;
    if (enablestat) {
        __sync_sub_and_fetch(&blckcount, size);
        __sync_sub_and_fetch(&counts[std::min((size_t)ORDERS - 1, order)], 1);
    }
    munmap(block, size);
}

void UnrefAtomic(void *block, int add, TAllocThreadState *tls) {
    if (block == 0) {
        return;
    }
    TBlockHeader *hdr = (TBlockHeader *)block;
    if (__sync_sub_and_fetch(&hdr->RefCount, add) == 0) {
        size_t index = ++counter % ORDERS;
        void *data[1] = {0};
        size_t count = PopPages(index, data, 1);
        if (count) {
            UnMap(data[0], index);
        }
        size_t order = hdr->Size / PAGE_SIZE;
        if (order == CHUNK_ORDER && tls) {
            if (tls->PageNum < 8) {
                tls->Pages[tls->PageNum++] = block;
                return;
            }
        }
        if (!PushPage(block, order)) {
            UnMap(block, order);
        }
    }
}

void CreateAllocatorKey() {
    if (__sync_bool_compare_and_swap(&init, 0, 1)) {
        pthread_key_create(&allocatorKey, Destructor);
        init = 2;
    }
}

static TAllocThreadState fake;
static long long lock = 0;

void SetKey() {
    if (__sync_bool_compare_and_swap(&lock, 0, 1)) {
        TAllocThreadState *tls = (TAllocThreadState *)malloc(sizeof(TAllocThreadState));
        memset(tls, 0, sizeof(TAllocThreadState));
        tls->AllocLimit = SINGLE_ALLOC;
        pthread_setspecific(allocatorKey, (void *)tls);
        lock = 0;
    }
}

void UnRef(void *block, int counter, TAllocThreadState *tls) {
    if (tls == 0) {
        UnrefAtomic(block, counter, 0);
        return;
    }
    if (tls->Block == block) {
        tls->Counter += counter;
    } else {
        UnrefAtomic(tls->Block, tls->Counter, tls);
        tls->Block = block;
        tls->Counter = counter;
    }
}

size_t Encode(size_t size, size_t off) {
    size_t ptr = off / PAGE_SIZE;
    return (size << 8) + (ptr & 0xff);
}


void Destructor(void *data) {
    TAllocThreadState *tls = (TAllocThreadState *)data;
    if (tls->Chunk) {
        TBlockHeader &hdr = *(TBlockHeader *)tls->Chunk;
        UnrefAtomic(tls->Chunk, CHUNK_SIZE - hdr.AllCount, 0);
    }
    UnrefAtomic(tls->Block, tls->Counter, 0);
    for (size_t i = 0; i < tls->PageNum; ++i) {
        UnMap(tls->Pages[i], CHUNK_ORDER);
    }
    pthread_setspecific(allocatorKey, 0);
    free(data);
}

extern "C" void *malloc(size_t size) {
    TAllocThreadState *tls = &fake;
    if (init < 2) {
        CreateAllocatorKey();
    } else {
        tls = (TAllocThreadState *)pthread_getspecific(allocatorKey);
        if (tls == 0) {
            SetKey();
            tls = &fake;
        }
    }
    size = Align(size, sizeof(size_t));
    size_t extsize = size + sizeof(size_t) + sizeof(TBlockHeader);
    if (extsize > tls->AllocLimit) {
        void *block = SysAlloc(extsize);
        TBlockHeader *hdr = (TBlockHeader *)block;
        hdr->RefCount = 1;
        hdr->Size = extsize;
        hdr->AllCount = 0;
        size_t *allocMark = (size_t *)Advance(block, sizeof(TBlockHeader));
        *allocMark = Encode(size, 0);
        if (enablestat) {
            __sync_add_and_fetch(&miss, 1);
            __sync_add_and_fetch(&allocount, 1);
            __sync_add_and_fetch(&memocount, size);
        }
        return allocMark + 1;
    }
    size_t ptr = tls->Ptr;
    void *chunk = tls->Chunk;

    if (ptr < extsize) {
        if (chunk) {
            TBlockHeader &hdr = *(TBlockHeader *)chunk;
            UnRef(chunk, CHUNK_SIZE - hdr.AllCount, tls);
        }
        if (tls->PageNum == 0) {
            tls->PageNum = PopPages(CHUNK_ORDER, tls->Pages, 8);
            if (tls->PageNum == 0) {
                tls->PageNum = LowLevelAlloc(CHUNK_ORDER, tls->Pages, 8);
            }
        }
        chunk = tls->Pages[--tls->PageNum];
        TBlockHeader &hdr = *(TBlockHeader *)chunk;
        hdr.RefCount = CHUNK_SIZE;
        hdr.Size = CHUNK_SIZE;
        hdr.AllCount = 0;
        tls->Chunk = chunk;
        ptr = CHUNK_SIZE;
    }
    ptr -= size + sizeof(size_t);
    size_t *allocMark = (size_t *)Advance(chunk, ptr);
    allocMark[0] = Encode(size, ptr);
    TBlockHeader *hdr = (TBlockHeader *)chunk;
    ++hdr->AllCount;
    if (enablestat) {
        __sync_add_and_fetch(&miss, 1);
        __sync_add_and_fetch(&allocount, 1);
        __sync_add_and_fetch(&memocount, size);
    }
    tls->Ptr = ptr;
    return allocMark + 1;
}

extern "C" void free(void *ptr) {
    if (ptr == 0)
        return;

    size_t *allocMark = ((size_t *)ptr) - 1;
    size_t size = allocMark[0];
    size_t block = size_t(allocMark) & ~(PAGE_SIZE - 1);
    block = block - ((size & 0xff) * PAGE_SIZE);

    if (enablestat) {
        __sync_sub_and_fetch(&allocount, 1);
        __sync_sub_and_fetch(&memocount, size >> 8);
    }

    UnRef((void *)block, 1, (TAllocThreadState *)pthread_getspecific(allocatorKey));
}

#define OP_THROWNOTHING throw()
#define OP_THROWBADALLOC throw (std::bad_alloc)

void *operator new(size_t size) OP_THROWBADALLOC {
    return malloc(size);
}

void *operator new(size_t size, const std::nothrow_t&) OP_THROWNOTHING {
    return malloc(size);
}

void operator delete(void *p) OP_THROWNOTHING {
    free(p);
}

void operator delete(void *p, const std::nothrow_t&) OP_THROWNOTHING {
    free(p);
}

void *operator new[](size_t size) OP_THROWBADALLOC {
    return malloc(size);
}

void *operator new[](size_t size, const std::nothrow_t&) OP_THROWNOTHING {
    return malloc(size);
}

void operator delete[](void *p) OP_THROWNOTHING {
    free(p);
}

void operator delete[](void *p, const std::nothrow_t&) OP_THROWNOTHING {
    free(p);
}

extern "C" void *calloc(size_t n, size_t elem_size) {
    const size_t size = n * elem_size;
    if (elem_size != 0 && size / elem_size != n) {
        return 0;
    }
    void *result = malloc(size);
    if (result != 0) {
        memset(result, 0, size);
    }
    return result;
}

extern "C" void cfree(void *ptr) {
    free(ptr);
}

extern "C" void *realloc(void *oldPtr, size_t newSize) {
    if (oldPtr == 0) {
        void *result = malloc(newSize);
        return result;
    }
    if (newSize == 0) {
        free(oldPtr);
        return 0;
    }

    void *newPtr = malloc(newSize);
    if (newPtr == 0) {
        return 0;
    }
    size_t size = ((size_t *)oldPtr)[-1];
    size_t oldSize = size >> 8;
    memcpy(newPtr, oldPtr, std::min(oldSize,newSize));
    free(oldPtr);
    return newPtr;
}


