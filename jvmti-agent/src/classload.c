#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <sys/stat.h>
#include <arpa/inet.h>

#include "classload.h"

typedef struct {
   uint16_t first;
   uint16_t second; 
} CPIndexPair;

static uint32_t read_uint32(const uint8_t* buffer, size_t* buffer_pos) {
    uint32_t* uint32_ptr = (uint32_t*)(buffer + *buffer_pos);
    *buffer_pos += sizeof(uint32_t);

    return htonl(*uint32_ptr);
}

static uint16_t read_uint16(const uint8_t* buffer, size_t* buffer_pos) {
    uint16_t* uint16_ptr = (uint16_t*)(buffer + *buffer_pos);
    *buffer_pos += sizeof(uint16_t);

    return htons(*uint16_ptr);
}

static char read_byte(const uint8_t* buffer, size_t* buffer_pos) {
    char byte = buffer[*buffer_pos];
    *buffer_pos += sizeof(char);

    return byte;
}

static inline CPEntry* get_cp_entry(CPool* const_pool, int cp_entry_idx) {
    return const_pool->entries + cp_entry_idx;
}

static bool read_utf8_const_pool_entry(int cp_entry_idx, CPool* const_pool, const uint8_t* buffer, size_t* buffer_pos) {
    uint16_t utf8_length = read_uint16(buffer, buffer_pos);

    size_t utf8_buf_len = utf8_length + 1;
    uint8_t* utf8_buf = malloc(utf8_buf_len);
    if (utf8_buf == NULL) {
        return false;
    }

    memcpy(utf8_buf, buffer + *buffer_pos, utf8_length);
    utf8_buf[utf8_length] = '\0';

    CPEntry* utf8_cp_entry = get_cp_entry(const_pool, cp_entry_idx);
    utf8_cp_entry->tag = CPUtf8;
    utf8_cp_entry->value = utf8_buf;

    *buffer_pos += utf8_length;

    return true;
}

 // TODO create macro to declare constant pool entries parsing functions
static bool read_int_const_pool_entry(int cp_entry_idx, CPool* const_pool, const uint8_t* buffer, size_t* buffer_pos) {
   // allocating 4 bytes for integer constant
   uint32_t* int_buf = malloc(sizeof(uint32_t));
   if (int_buf == NULL) {
       return false;
   }

   uint32_t value = read_uint32(buffer, buffer_pos);

   *int_buf = value;

   CPEntry* int_cp_entry = get_cp_entry(const_pool, cp_entry_idx);
   int_cp_entry->tag = CPInteger;
   int_cp_entry->value = int_buf;

   return true;
}

static CPIndexPair* cp_index_pair_new(uint16_t first, uint16_t second) {
   CPIndexPair* index_pair_ptr = malloc(sizeof(CPIndexPair));
   if (index_pair_ptr == NULL) {
        return NULL;
   }

   index_pair_ptr->first = first;
   index_pair_ptr->second = second;

   return index_pair_ptr;
}

static bool read_method_ref_const_pool_entry(int cp_entry_idx, CPool* const_pool, const uint8_t* buffer, size_t* buffer_pos) {
   uint16_t class_index = read_uint16(buffer, buffer_pos);
   uint16_t name_type_index = read_uint16(buffer, buffer_pos);

   CPIndexPair* index_pair = cp_index_pair_new(class_index, name_type_index);
   if (index_pair == NULL) {
       return false;
   }

   CPEntry* method_ref_cp_entry = get_cp_entry(const_pool, cp_entry_idx);
   method_ref_cp_entry->tag = CPMethodRef;
   method_ref_cp_entry->value = index_pair;

   return true;
}

static bool read_nametype_const_pool_entry(int cp_entry_idx, CPool* const_pool, const uint8_t* buffer, size_t* buffer_pos) {
   uint16_t name_index = read_uint16(buffer, buffer_pos);
   uint16_t type_index = read_uint16(buffer, buffer_pos);

   CPIndexPair* index_pair = cp_index_pair_new(name_index, type_index);
   if (index_pair == NULL) {
        return false;
   }

   // inline func get_cp_entry
   CPEntry* nametype_cp_entry = get_cp_entry(const_pool, cp_entry_idx);
   nametype_cp_entry->tag = CPNameAndType;
   nametype_cp_entry->value = index_pair;

   return true;
}

static bool read_class_const_pool_entry(int cp_entry_idx, CPool* const_pool, const uint8_t* buffer, size_t* buffer_pos) {
    uint16_t* class_name_index_ptr = malloc(sizeof(uint16_t));
    if (class_name_index_ptr == NULL) {
        return false;
    }

    uint16_t class_name_index = read_uint16(buffer, buffer_pos);
    *class_name_index_ptr = class_name_index;

    CPEntry* class_cp_entry = get_cp_entry(const_pool, cp_entry_idx);
    class_cp_entry->tag = CPClass;
    class_cp_entry->value = class_name_index_ptr;

    return true;
}

static bool read_const_pool_entry(int cp_entry_idx, CPool* const_pool, const uint8_t* buffer, size_t* buffer_pos) {
    char cp_entry_tag = read_byte(buffer, buffer_pos);

    switch (cp_entry_tag) {
        case CPUtf8: return read_utf8_const_pool_entry(cp_entry_idx, const_pool, buffer, buffer_pos);
        case CPInteger: return read_int_const_pool_entry(cp_entry_idx, const_pool, buffer, buffer_pos);
        case CPMethodRef: return read_method_ref_const_pool_entry(cp_entry_idx, const_pool, buffer, buffer_pos);
        case CPClass: return read_class_const_pool_entry(cp_entry_idx, const_pool, buffer, buffer_pos);
        case CPNameAndType: return read_nametype_const_pool_entry(cp_entry_idx, const_pool, buffer, buffer_pos);
        default: 
            return false;
    }

    return true;
}

static char* get_class_name(int cp_entry_idx, CPool* const_pool) {
    CPEntry* class_cp_entry = get_cp_entry(const_pool, cp_entry_idx - 1);

    uint16_t* class_name_cp_entry_idx_ptr = (uint16_t*)class_cp_entry->value;
    CPEntry* class_name_cp_entry = get_cp_entry(const_pool, *class_name_cp_entry_idx_ptr - 1);

    return class_name_cp_entry->value;
}

JClass* jclass_load(const uint8_t* buffer) {
    size_t buffer_pos = 0;
    
    read_uint32(buffer, &buffer_pos); // magic number
    read_uint16(buffer, &buffer_pos); // minor version
    read_uint16(buffer, &buffer_pos); // major version

    uint16_t cp_size = read_uint16(buffer, &buffer_pos) - 1;
    size_t cp_obj_size = sizeof(CPool) + cp_size * sizeof(CPEntry);

    CPool* const_pool = malloc(cp_obj_size);
    if (const_pool == NULL) {
        return false;
    }

    memset(const_pool, 0, cp_obj_size);
    const_pool->size = cp_size;

    for (int cp_entry_idx = 0;cp_entry_idx < cp_size;cp_entry_idx++) {
        bool success = read_const_pool_entry(cp_entry_idx, const_pool, buffer, &buffer_pos);
        if (!success) {
            return false;
        }
    }

    read_uint16(buffer, &buffer_pos); // access flags

    uint16_t this_class_cp_entry_idx = read_uint16(buffer, &buffer_pos);
    char* class_name = get_class_name(this_class_cp_entry_idx, const_pool);

    JClass* jclass = malloc(sizeof(JClass));
    if (jclass == NULL) {
        return NULL;
    }

    jclass->name = class_name;
    jclass->const_pool = const_pool;
    
    return jclass;
}

void jclass_free(JClass* jclass) {
    CPool* const_pool = jclass->const_pool;
    for (int cp_entry_idx = 0;cp_entry_idx < const_pool->size;cp_entry_idx++) {
        CPEntry* cp_entry = get_cp_entry(const_pool, cp_entry_idx);
        free(cp_entry->value);
    }

    free(const_pool);
    free(jclass);
}