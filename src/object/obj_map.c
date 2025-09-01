#include "obj_map.h"
#include "class.h"
#include "common.h"
#include "header_obj.h"
#include "sparrow.h"
#include "utils.h"
#include "vm.h"
#include "obj_string.h"
#include "obj_range.h"

ObjMap* objmap_new(VM* vm) {
    ObjMap* map = ALLOCATE(vm, ObjMap);
    objheader_init(vm, &map->header, OT_MAP, vm->map_class);
    map->capacity = 0;
    map->len = 0;
    map->entries = NULL;
    return map;
}

static u32 hash_num(double num) {
    Bits64 bits;
    bits.bits_double = num;
    return bits.bits32[0] ^ bits.bits32[1];
}

static u32 hash_obj(ObjHeader* header) {
    switch (header->type) {
        case OT_CLASS:
            return hash_string(((Class*)header)->name->val.start, ((Class*)header)->name->val.len);

        case OT_RANGE: {
            ObjRange* range = (ObjRange*)header;
            return hash_num(range->from) ^ hash_num(range->to) ^ hash_num(range->step);
        }

        case OT_STRING:
            return ((ObjString*)header)->hash_code;

        default:
            RUNTIME_ERROR("unhashable object. type: %d", header->type);
    }
    return 0;
}

static u32 hash_value(Value val) {
    switch (val.type) {
        case VT_F64:
            return hash_num(val.f64val);
        case VT_I32:
            return hash_num(val.i32val);
        case VT_U32:
            return hash_num(val.u32val);
        case VT_U8:
            return hash_num(val.u8val);
        case VT_FALSE:
            return 0;
        case VT_TRUE:
            return 1;
        case VT_NULL:
            return 2;
        case VT_OBJ:
            return hash_obj(val.header);
        default:
            RUNTIME_ERROR("unhashable value. type: %d", val.type);
    }
    return 0;
}

static bool add_entry(Entry* entries, u32 capacity, Value key, Value val) {
    u32 index = hash_value(key) % capacity;
    while (true) {
        if (entries[index].key.type == VT_UNDEFINED) {
            entries[index].key = key;
            entries[index].val = val;
            return true;
        } else if (value_is_equal(entries[index].key, key)) {
            entries[index].val = val;
            return false;
        }
        index = (index + 1) % capacity;
    }
}

static void resize_map(VM* vm, ObjMap* map, u32 new_capacity) {
    Entry* new_entries = ALLOCATE_ARRAY(vm, Entry, new_capacity);

    for (int i = 0; i < new_capacity; i++) {
        new_entries[i].key = VT_TO_VALUE(VT_UNDEFINED);
        new_entries[i].val = VT_TO_VALUE(VT_FALSE);
    }

    if (new_capacity > 0) {
        for (int i = 0; i < map->capacity; i++) {
            if (map->entries[i].key.type == VT_UNDEFINED) {
                continue;
            }

            add_entry(new_entries, new_capacity, map->entries[i].key, map->entries[i].val);
        }
    }

    DEALLOCATE_ARRAY(vm, map->entries, map->capacity);
    map->entries = new_entries;
    map->capacity = new_capacity;
}

static Entry* find_entry(ObjMap* map, Value key) {
    if (map->capacity == 0) {
        return NULL;
    }

    u32 index = hash_value(key) % map->capacity;
    Entry* entry = NULL;
    while (true) {
        entry = &map->entries[index];
        
        if (value_is_equal(key, entry->key)) {
            return entry;
        }

        if (VALUE_IS_UNDEFINED(entry->key) && VALUE_IS_FALSE(entry->val)) {
            return NULL;
        }

        index = (index + 1) % map->capacity;
    }
    return NULL;
}

void objmap_set(VM* vm, ObjMap* map, Value key, Value val) {
    if (map->len + 1 > map->capacity * MAP_LOAD_PRECENT) {
        u32 new_capacity = map->capacity * CAPACITY_GROW_FACTOR;
        new_capacity = new_capacity < MIN_CAPACITY ? MIN_CAPACITY : new_capacity;
        resize_map(vm, map, new_capacity);
    }

    // 返回true，表示创建了新entry
    if (add_entry(map->entries, map->capacity, key, val)) {
        map->len++;
    }
}

Value objmap_get(ObjMap* map, Value key) {
    Entry* entry = find_entry(map, key);
    return entry == NULL ? VT_TO_VALUE(VT_UNDEFINED) : entry->val;
}

void objmap_clear(VM* vm, ObjMap* map) {
    DEALLOCATE_ARRAY(vm, map->entries, map->capacity);
    map->entries = NULL;
    map->capacity = 0;
    map->len = 0;
}

Value objmap_remove(VM* vm, ObjMap* map, Value key) {
    Entry* entry = find_entry(map, key);
    if (entry == NULL) {
        return VT_TO_VALUE(VT_NULL);
    }

    Value val = entry->val;
    if (VALUE_IS_OBJ(val)) {
        push_tmp_root(vm, val.header);
    }
    
    entry->key = VT_TO_VALUE(VT_UNDEFINED);
    entry->val = VT_TO_VALUE(VT_TRUE);

    map->len--;

    if (map->len == 0) {
        objmap_clear(vm, map);
    } else if (map->len < ((f64)map->capacity / CAPACITY_GROW_FACTOR * MAP_LOAD_PRECENT) && map->len > MIN_CAPACITY) {
        u32 new_capacity = map->capacity / CAPACITY_GROW_FACTOR;
        new_capacity = new_capacity < MIN_CAPACITY ? MIN_CAPACITY : new_capacity;
        resize_map(vm, map, new_capacity);
    }

    if (VALUE_IS_OBJ(val)) {
        pop_tmp_root(vm);
    }

    return val;
}
