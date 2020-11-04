/*
 * Copyright 2020 Andrei Pangin
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string.h>
#include <unistd.h>
#include "callTraceStorage.h"
#include "os.h"


static const u32 INITIAL_CAPACITY = 65536;
static const u32 CALL_TRACE_CHUNK = 8 * 1024 * 1024;
static const size_t PAGE_ALIGNMENT = sysconf(_SC_PAGESIZE) - 1;


class LongHashTable {
  private:
    LongHashTable* _prev;
    void* _padding0;
    u32 _capacity;
    u32 _padding1[15];
    volatile u32 _size;
    u32 _padding2[15];

    static size_t getSize(u32 capacity) {
        size_t size = sizeof(LongHashTable) + (sizeof(u64) + sizeof(void*)) * capacity;
        return (size + PAGE_ALIGNMENT) & ~PAGE_ALIGNMENT;
    }

  public:
    static LongHashTable* allocate(LongHashTable* prev, u32 capacity) {
        LongHashTable* table = (LongHashTable*)OS::safeAlloc(getSize(capacity));
        if (table != NULL) {
            table->_prev = prev;
            table->_capacity = capacity;
            table->_size = 0;
        }
        return table;
    }

    LongHashTable* destroy() {
        LongHashTable* prev = _prev;
        OS::safeFree(this, getSize(_capacity));
        return prev;
    }

    LongHashTable* prev() {
        return _prev;
    }

    u32 capacity() {
        return _capacity;
    }

    u32 size() {
        return _size;
    }

    u32 incSize() {
        return __sync_add_and_fetch(&_size, 1);
    }

    u64* keys() {
        return (u64*)(this + 1);
    }

    CallTrace** values() {
        return (CallTrace**)(keys() + _capacity);
    }

    void clear() {
        memset(keys(), 0, (sizeof(u64) + sizeof(void*)) * _capacity);
        _size = 0;
    }
};


CallTraceStorage::CallTraceStorage() : _allocator(CALL_TRACE_CHUNK) {
    _current_table = LongHashTable::allocate(NULL, INITIAL_CAPACITY);
}

CallTraceStorage::~CallTraceStorage() {
    while (_current_table != NULL) {
        _current_table = _current_table->destroy();
    }
}

void CallTraceStorage::clear() {
    while (_current_table->prev() != NULL) {
        _current_table = _current_table->destroy();
    }
    _current_table->clear();
    _allocator.clear();
}

void CallTraceStorage::collect(std::map<u32, CallTrace*>& map) {
    for (LongHashTable* table = _current_table; table != NULL; table = table->prev()) {
        u64* keys = table->keys();
        CallTrace** values = table->values();
        u32 capacity = table->capacity();

        for (u32 slot = 0; slot < capacity; slot++) {
            if (keys[slot] != 0) {
                map[capacity - (INITIAL_CAPACITY - 1) + slot] = values[slot];
            }
        }
    }
}

// Adaptation of MurmurHash64A by Austin Appleby
u64 CallTraceStorage::calcHash(int num_frames, ASGCT_CallFrame* frames) {
    const u64 M = 0xc6a4a7935bd1e995ULL;
    const int R = 47;

    int len = num_frames * sizeof(ASGCT_CallFrame);
    u64 h = len * M;

    const u64* data = (const u64*)frames;
    const u64* end = data + len / 8;

    while (data != end) {
        u64 k = *data++;
        k *= M;
        k ^= k >> R;
        k *= M;
        h ^= k;
        h *= M;
    }

    if (len & 4) {
        h ^= *(u32*)data;
        h *= M;
    }

    h ^= h >> R;
    h *= M;
    h ^= h >> R;

    return h;
}

CallTrace* CallTraceStorage::storeCallTrace(int num_frames, ASGCT_CallFrame* frames) {
    const size_t header_size = sizeof(CallTrace) - sizeof(ASGCT_CallFrame);
    CallTrace* buf = (CallTrace*)_allocator.alloc(header_size + num_frames * sizeof(ASGCT_CallFrame));
    if (buf != NULL) {
        buf->num_frames = num_frames;
        // Do not use memcpy inside signal handler
        for (int i = 0; i < num_frames; i++) {
            buf->frames[i] = frames[i];
        }
    }
    return buf;
}

CallTrace* CallTraceStorage::findCallTrace(LongHashTable* table, u64 hash) {
    u64* keys = table->keys();
    u32 capacity = table->capacity();
    u32 slot = hash & (capacity - 1);
    u32 step = 0;

    while (keys[slot] != hash) {
        if (keys[slot] == 0) {
            return NULL;
        }
        if (++step >= capacity) {
            return NULL;
        }
        slot = (slot + step) & (capacity - 1);
    }

    return table->values()[slot];
}

u32 CallTraceStorage::put(int num_frames, ASGCT_CallFrame* frames) {
    u64 hash = calcHash(num_frames, frames);

    LongHashTable* table = _current_table;
    u64* keys = table->keys();
    u32 capacity = table->capacity();
    u32 slot = hash & (capacity - 1);
    u32 step = 0;

    while (keys[slot] != hash) {
        if (keys[slot] == 0) {
            if (!__sync_bool_compare_and_swap(&keys[slot], 0, hash)) {
                continue;
            }

            // Increment the table size, and if the load factor exceeds 0.75, reserve a new table
            if (table->incSize() == capacity * 3 / 4) {
                LongHashTable* new_table = LongHashTable::allocate(table, capacity * 2);
                if (new_table != NULL) {
                    __sync_bool_compare_and_swap(&_current_table, table, new_table);
                }
            }

            // Migrate from a previous table to save space
            CallTrace* trace = table->prev() == NULL ? NULL : findCallTrace(table->prev(), hash);
            if (trace == NULL) {
                trace = storeCallTrace(num_frames, frames);
            }
            table->values()[slot] = trace;
            break;
        }

        if (++step >= capacity) {
            // Very unlikely case of a table overflow
            return 0;
        }
        // Improved version of linear probing
        slot = (slot + step) & (capacity - 1);
    }

    return capacity - (INITIAL_CAPACITY - 1) + slot;
}