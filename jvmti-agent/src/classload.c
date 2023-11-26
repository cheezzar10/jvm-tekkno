#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <sys/stat.h>
#include <arpa/inet.h>

#include "classload.h"

// reusing agent logging facility
void log_debug(const char* format, ...);

typedef struct {
   uint16_t first;
   uint16_t second; 
} CPIndexPair;

static const int CP_SLOT_STOP = 0;
static const int CP_SLOT_NEXT = 1;

static uint64_t read_uint64(const uint8_t* buffer, size_t* buffer_pos) {
    uint64_t* uint64_ptr = (uint64_t*)(buffer + *buffer_pos);
    *buffer_pos += sizeof(uint64_t);

    uint64_t uint64_high_bytes = htonl(*uint64_ptr >> 32);
    uint64_t uint64_low_bytes = htonl(*uint64_ptr & 0x00000000FFFFFFFF);

    return (uint64_low_bytes << 32) | uint64_high_bytes;
}

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

static int read_utf8_const_pool_entry(int cp_entry_idx, CPool* const_pool, const uint8_t* buffer, size_t* buffer_pos) {
    uint16_t utf8_length = read_uint16(buffer, buffer_pos);

    size_t utf8_buf_len = utf8_length + 1;
    char* utf8_buf = malloc(utf8_buf_len);
    if (utf8_buf == NULL) {
        log_debug("UTF8 buffer allocation failed");
        return CP_SLOT_STOP;
    }

    memcpy(utf8_buf, buffer + *buffer_pos, utf8_length);
    utf8_buf[utf8_length] = '\0';

    CPEntry* utf8_cp_entry = get_cp_entry(const_pool, cp_entry_idx);
    utf8_cp_entry->tag = CPUtf8;
    utf8_cp_entry->value = utf8_buf;

    *buffer_pos += utf8_length;

    return CP_SLOT_NEXT;
}

 // TODO create macro to declare constant pool entries parsing functions
static int read_int_const_pool_entry(int cp_entry_idx, CPool* const_pool, const uint8_t* buffer, size_t* buffer_pos) {
   // allocating 4 bytes for integer constant
   uint32_t* int_buf = malloc(sizeof(uint32_t));
   if (int_buf == NULL) {
       log_debug("int buffer allocation failed");
       return CP_SLOT_STOP;
   }

   uint32_t value = read_uint32(buffer, buffer_pos);

   *int_buf = value;

   CPEntry* int_cp_entry = get_cp_entry(const_pool, cp_entry_idx);
   int_cp_entry->tag = CPInteger;
   int_cp_entry->value = int_buf;

   return CP_SLOT_NEXT;
}

static int read_long_const_pool_entry(int cp_entry_idx, CPool* const_pool, const uint8_t* buffer, size_t* buffer_pos) {
    // allocating 8 bytes for long constant
    uint64_t* long_buf = malloc(sizeof(uint64_t));
    if (long_buf == NULL) {
        log_debug("long buffer allocation failed");
        return CP_SLOT_STOP;
    }

    uint64_t value = read_uint64(buffer, buffer_pos);
    *long_buf = value;

    CPEntry* long_cp_entry = get_cp_entry(const_pool, cp_entry_idx);
    long_cp_entry-> tag = CPLong;
    long_cp_entry->value = long_buf;

    // Long constant pool entry takes 2 consecutive slots
    return CP_SLOT_NEXT + CP_SLOT_NEXT;
}

static int read_double_const_pool_entry(int cp_entry_idx, CPool* const_pool, const uint8_t* buffer, size_t* buffer_pos) {
    double* double_buf = malloc(sizeof(double));
    if (double_buf == NULL) {
        log_debug("double value buffer allocation failed");
        return CP_SLOT_STOP;
    }

    uint64_t value = read_uint64(buffer, buffer_pos);
    memcpy(double_buf, &value, sizeof(double));

    CPEntry* double_cp_entry = get_cp_entry(const_pool, cp_entry_idx);
    double_cp_entry->tag = CPDouble;
    double_cp_entry->value = double_buf;

    // each Double constant pool occupy two consecutive slots
    return CP_SLOT_NEXT + CP_SLOT_NEXT;
}

static int read_float_const_pool_entry(int cp_entry_idx, CPool* const_pool, const uint8_t* buffer, size_t* buffer_pos) {
    float* float_buf = malloc(sizeof(float));
    if (float_buf == NULL) {
        log_debug("float buffer allocation failed");
        return CP_SLOT_STOP;
    }

    uint32_t value = read_uint32(buffer, buffer_pos);
    memcpy(float_buf, &value, sizeof(float));

    CPEntry* float_cp_entry = get_cp_entry(const_pool, cp_entry_idx);
    float_cp_entry->tag = CPFloat;
    float_cp_entry->value = float_buf;

    return CP_SLOT_NEXT;
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

static int read_ref_const_pool_entry(CPTag cp_entry_tag, int cp_entry_idx, CPool* const_pool, const uint8_t* buffer, size_t* buffer_pos) {
   uint16_t class_index = read_uint16(buffer, buffer_pos);
   uint16_t name_type_index = read_uint16(buffer, buffer_pos);

   CPIndexPair* index_pair = cp_index_pair_new(class_index, name_type_index);
   if (index_pair == NULL) {
       log_debug("index pair allocation failed");
       return CP_SLOT_STOP;
   }

   CPEntry* ref_cp_entry = get_cp_entry(const_pool, cp_entry_idx);
   ref_cp_entry->tag = cp_entry_tag;
   ref_cp_entry->value = index_pair;

   return CP_SLOT_NEXT;
}

static int read_nametype_const_pool_entry(int cp_entry_idx, CPool* const_pool, const uint8_t* buffer, size_t* buffer_pos) {
   uint16_t name_index = read_uint16(buffer, buffer_pos);
   uint16_t type_index = read_uint16(buffer, buffer_pos);

   CPIndexPair* index_pair = cp_index_pair_new(name_index, type_index);
   if (index_pair == NULL) {
       log_debug("index pair allocation failed");
        return CP_SLOT_STOP;
   }

   // inline func get_cp_entry
   CPEntry* nametype_cp_entry = get_cp_entry(const_pool, cp_entry_idx);
   nametype_cp_entry->tag = CPNameAndType;
   nametype_cp_entry->value = index_pair;

   return CP_SLOT_NEXT;
}

static int read_method_handle_const_pool_entry(int cp_entry_idx, CPool* const_pool, const uint8_t* buffer, size_t* buffer_pos) {
    char method_handle_tag = read_byte(buffer, buffer_pos);
    uint16_t ref_index = read_uint16(buffer, buffer_pos);

    CPIndexPair* tag_ref_index_pair = cp_index_pair_new(method_handle_tag, ref_index);
    if (tag_ref_index_pair == NULL) {
        log_debug("(method handle tag, ref) pair allocation failed");
        return CP_SLOT_STOP;
    }

    CPEntry* method_handle_cp_entry = get_cp_entry(const_pool, cp_entry_idx);
    method_handle_cp_entry->tag = CPMethodHandle;
    method_handle_cp_entry->value = tag_ref_index_pair;

    return CP_SLOT_NEXT;
}

static int read_utf8_ref_const_pool_entry(CPTag cp_entry_tag, int cp_entry_idx, CPool* const_pool, const uint8_t* buffer, size_t* buffer_pos) {
    uint16_t* utf8_ref_cp_entry_index_ptr = malloc(sizeof(uint16_t));
    if (utf8_ref_cp_entry_index_ptr == NULL) {
        log_debug("UTF-8 entry index buffer allocation failed");
        return CP_SLOT_STOP;
    }

    uint16_t utf8_ref_cp_entry_index = read_uint16(buffer, buffer_pos);
    *utf8_ref_cp_entry_index_ptr = utf8_ref_cp_entry_index;

    CPEntry* utf8_ref_cp_entry = get_cp_entry(const_pool, cp_entry_idx);
    utf8_ref_cp_entry->tag = cp_entry_tag;
    utf8_ref_cp_entry->value = utf8_ref_cp_entry_index_ptr;

    return CP_SLOT_NEXT;
}

static int read_const_pool_entry(int cp_entry_idx, CPool* const_pool, const uint8_t* buffer, size_t* buffer_pos) {
    char cp_entry_tag = read_byte(buffer, buffer_pos);

    switch (cp_entry_tag) {
        case CPUtf8: return read_utf8_const_pool_entry(cp_entry_idx, const_pool, buffer, buffer_pos);
        case CPInteger: return read_int_const_pool_entry(cp_entry_idx, const_pool, buffer, buffer_pos);
        case CPFloat: return read_float_const_pool_entry(cp_entry_idx, const_pool, buffer, buffer_pos);
        case CPLong: return read_long_const_pool_entry(cp_entry_idx, const_pool, buffer, buffer_pos);
        case CPDouble: return read_double_const_pool_entry(cp_entry_idx, const_pool, buffer, buffer_pos);
        case CPClass: return read_utf8_ref_const_pool_entry(CPClass, cp_entry_idx, const_pool, buffer, buffer_pos);
        case CPString: return read_utf8_ref_const_pool_entry(CPString, cp_entry_idx, const_pool, buffer, buffer_pos);
        case CPFieldRef: return read_ref_const_pool_entry(CPFieldRef, cp_entry_idx, const_pool, buffer, buffer_pos);
        case CPMethodRef: return read_ref_const_pool_entry(CPMethodRef, cp_entry_idx, const_pool, buffer, buffer_pos);
        case CPInterfaceMethodRef: return read_ref_const_pool_entry(CPInterfaceMethodRef, cp_entry_idx, const_pool, buffer, buffer_pos);
        case CPNameAndType: return read_nametype_const_pool_entry(cp_entry_idx, const_pool, buffer, buffer_pos);
        case CPMethodHandle: return read_method_handle_const_pool_entry(cp_entry_idx, const_pool, buffer, buffer_pos);
        case CPMethodType: return read_utf8_ref_const_pool_entry(CPMethodType, cp_entry_idx, const_pool, buffer, buffer_pos);
        case CPInvokeDynamic: return read_ref_const_pool_entry(CPInvokeDynamic, cp_entry_idx, const_pool, buffer, buffer_pos); // fix, first arg is BSM index
        default: 
            log_debug("unknown constant pool entry tag: %d at index %d\n", cp_entry_tag, cp_entry_idx);
            return CP_SLOT_STOP;
    }
}

static char* get_class_name(int cp_entry_idx, CPool* const_pool) {
    CPEntry* class_cp_entry = get_cp_entry(const_pool, cp_entry_idx - 1);

    uint16_t* class_name_cp_entry_idx_ptr = (uint16_t*)class_cp_entry->value;
    CPEntry* class_name_cp_entry = get_cp_entry(const_pool, *class_name_cp_entry_idx_ptr - 1);

    return class_name_cp_entry->value;
}

JClass* jclass_load(const uint8_t* buffer) {
    size_t buffer_pos = 0;
    
    uint32_t magic_number = read_uint32(buffer, &buffer_pos);
    log_debug("magic number: %X", magic_number);

    uint16_t minor_version = read_uint16(buffer, &buffer_pos);
    log_debug("minor version: %d", minor_version);

    uint16_t major_version = read_uint16(buffer, &buffer_pos);
    log_debug("major version: %d", major_version);

    uint16_t cp_size = read_uint16(buffer, &buffer_pos) - 1;
    log_debug("constant pool size: %d", cp_size);

    size_t cp_obj_size = sizeof(CPool) + cp_size * sizeof(CPEntry);
    CPool* const_pool = malloc(cp_obj_size);
    if (const_pool == NULL) {
        log_debug("constant pool allocation failed");
        return NULL;
    }

    memset(const_pool, 0, cp_obj_size);
    const_pool->size = cp_size;

    for (int cp_entry_idx = 0;cp_entry_idx < cp_size;) {
        int next_cp_entry_distance = read_const_pool_entry(cp_entry_idx, const_pool, buffer, &buffer_pos);
        if (next_cp_entry_distance == 0) {
            return NULL;
        }

        cp_entry_idx += next_cp_entry_distance;
    }

    uint16_t access_flags = read_uint16(buffer, &buffer_pos);
    log_debug("access flags: %X", access_flags);

    uint16_t this_class_cp_entry_idx = read_uint16(buffer, &buffer_pos);
    char* class_name = get_class_name(this_class_cp_entry_idx, const_pool);
    log_debug("class name: %s", class_name);

    JClass* jclass = malloc(sizeof(JClass));
    if (jclass == NULL) {
        log_debug("JClass allocation failed");
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