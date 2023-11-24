#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <sys/stat.h>
#include <arpa/inet.h>

typedef enum {
    CPUtf8 = 1,
    CPInteger = 3,
    CPFloat = 4,
    CPLong = 5,
    CPDouble = 6,
    CPClass = 7,
    CPString = 8,
    CPFieldRef = 9,
    CPMethodRef = 10,
    CPInterfaceMethodRef = 11,
    CPNameAndType = 12
} CPTag;

typedef struct {
    CPTag tag;
    void* value;
} CPEntry;

typedef struct {
    size_t size;
    CPEntry entries[];
} CPool;

typedef struct {
   uint16_t first;
   uint16_t second; 
} CPIndexPair;

typedef struct {
    char* name;
    CPool* const_pool;
} JClass;

static long get_file_size(const char* file_path) {
    struct stat file_stat;

    int status = stat(file_path, &file_stat);
    if (status == -1) {
        return -1;
    }

    return file_stat.st_size;
}

static uint32_t read_uint32(const char* buffer, size_t* buffer_pos) {
    uint32_t* uint32_ptr = (uint32_t*)(buffer + *buffer_pos);
    *buffer_pos += sizeof(uint32_t);

    return htonl(*uint32_ptr);
}

static uint16_t read_uint16(const char* buffer, size_t* buffer_pos) {
    uint16_t* uint16_ptr = (uint16_t*)(buffer + *buffer_pos);
    *buffer_pos += sizeof(uint16_t);

    return htons(*uint16_ptr);
}

static char read_byte(const char* buffer, size_t* buffer_pos) {
    char byte = buffer[*buffer_pos];
    *buffer_pos += sizeof(char);

    return byte;
}

static inline CPEntry* get_cp_entry(CPool* const_pool, int cp_entry_idx) {
    return const_pool->entries + cp_entry_idx;
}

static bool read_utf8_const_pool_entry(int cp_entry_idx, CPool* const_pool, const char* buffer, size_t* buffer_pos) {
    uint16_t utf8_length = read_uint16(buffer, buffer_pos);

    size_t utf8_buf_len = utf8_length + 1;
    char* utf8_buf = malloc(utf8_buf_len);
    if (utf8_buf == NULL) {
        fprintf(stderr, "UTF8 buffer allocation failed\n");
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
static bool read_int_const_pool_entry(int cp_entry_idx, CPool* const_pool, const char* buffer, size_t* buffer_pos) {
   // allocating 4 bytes for integer constant
   uint32_t* int_buf = malloc(sizeof(uint32_t));
   if (int_buf == NULL) {
       fprintf(stderr, "int buffer allocation failed\n");
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

static bool read_method_ref_const_pool_entry(int cp_entry_idx, CPool* const_pool, const char* buffer, size_t* buffer_pos) {
   uint16_t class_index = read_uint16(buffer, buffer_pos);
   uint16_t name_type_index = read_uint16(buffer, buffer_pos);

   CPIndexPair* index_pair = cp_index_pair_new(class_index, name_type_index);
   if (index_pair == NULL) {
       fprintf(stderr, "index pair allocation failed\n");
       return false;
   }

   CPEntry* method_ref_cp_entry = get_cp_entry(const_pool, cp_entry_idx);
   method_ref_cp_entry->tag = CPMethodRef;
   method_ref_cp_entry->value = index_pair;

   return true;
}

static bool read_nametype_const_pool_entry(int cp_entry_idx, CPool* const_pool, const char* buffer, size_t* buffer_pos) {
   uint16_t name_index = read_uint16(buffer, buffer_pos);
   uint16_t type_index = read_uint16(buffer, buffer_pos);

   CPIndexPair* index_pair = cp_index_pair_new(name_index, type_index);
   if (index_pair == NULL) {
       fprintf(stderr, "index pair allocation failed\n");
        return false;
   }

   // inline func get_cp_entry
   CPEntry* nametype_cp_entry = get_cp_entry(const_pool, cp_entry_idx);
   nametype_cp_entry->tag = CPNameAndType;
   nametype_cp_entry->value = index_pair;

   return true;
}

static bool read_class_const_pool_entry(int cp_entry_idx, CPool* const_pool, const char* buffer, size_t* buffer_pos) {
    uint16_t* class_name_index_ptr = malloc(sizeof(uint16_t));
    if (class_name_index_ptr == NULL) {
        fprintf(stderr, "class name index buffer allocation failed\n");
        return false;
    }

    uint16_t class_name_index = read_uint16(buffer, buffer_pos);
    *class_name_index_ptr = class_name_index;

    CPEntry* class_cp_entry = get_cp_entry(const_pool, cp_entry_idx);
    class_cp_entry->tag = CPClass;
    class_cp_entry->value = class_name_index_ptr;

    return true;
}

static bool read_const_pool_entry(int cp_entry_idx, CPool* const_pool, const char* buffer, size_t* buffer_pos) {
    char cp_entry_tag = read_byte(buffer, buffer_pos);

    switch (cp_entry_tag) {
        case CPUtf8: return read_utf8_const_pool_entry(cp_entry_idx, const_pool, buffer, buffer_pos);
        case CPInteger: return read_int_const_pool_entry(cp_entry_idx, const_pool, buffer, buffer_pos);
        case CPMethodRef: return read_method_ref_const_pool_entry(cp_entry_idx, const_pool, buffer, buffer_pos);
        case CPClass: return read_class_const_pool_entry(cp_entry_idx, const_pool, buffer, buffer_pos);
        case CPNameAndType: return read_nametype_const_pool_entry(cp_entry_idx, const_pool, buffer, buffer_pos);
        default: 
            fprintf(stderr, "unknown constant pool entry tag: %d\n", cp_entry_tag);
            return false;
    }

    return true;
}

static void print_class_cp_entry(CPool* const_pool, CPEntry* class_cp_entry) {
    uint16_t* class_name_cp_entry_index_ptr = (uint16_t*)class_cp_entry->value;
    CPEntry* class_name_cp_entry = get_cp_entry(const_pool, *class_name_cp_entry_index_ptr - 1);

    printf("%s:Class", (char*)class_name_cp_entry->value);
}

static char* get_class_name(int cp_entry_idx, CPool* const_pool) {
    CPEntry* class_cp_entry = get_cp_entry(const_pool, cp_entry_idx - 1);

    uint16_t* class_name_cp_entry_idx_ptr = (uint16_t*)class_cp_entry->value;
    CPEntry* class_name_cp_entry = get_cp_entry(const_pool, *class_name_cp_entry_idx_ptr - 1);

    return class_name_cp_entry->value;
}

static void print_nametype_cp_entry(CPool* const_pool, CPEntry* nametype_cp_entry) {
    CPIndexPair* name_type_pair = (CPIndexPair*)nametype_cp_entry->value;

    CPEntry* name_cp_entry = get_cp_entry(const_pool, name_type_pair->first - 1);
    CPEntry* type_cp_entry = get_cp_entry(const_pool, name_type_pair->second - 1);

    printf("%s%s:NameType", (char*)name_cp_entry->value, (char*)type_cp_entry->value);
}

static void print_method_ref_cp_entry(CPool* const_pool, CPEntry* method_ref_cp_entry) {
    CPIndexPair* class_method_pair = (CPIndexPair*)method_ref_cp_entry->value;

    CPEntry* class_cp_entry = get_cp_entry(const_pool, class_method_pair->first - 1);
    print_class_cp_entry(const_pool, class_cp_entry);
    printf(".");

    CPEntry* nametype_cp_entry = get_cp_entry(const_pool, class_method_pair->second - 1);
    print_nametype_cp_entry(const_pool, nametype_cp_entry);

    printf(" : MethodRef");
}

static void print_cp_entry(int cp_entry_idx, CPool* const_pool) {
    CPEntry* cp_entry = get_cp_entry(const_pool, cp_entry_idx);
    printf("const pool entry [%d] = ", cp_entry_idx);

    switch (cp_entry->tag) {
        case CPUtf8: printf("%s:Utf8", (char*)cp_entry->value); break;
        case CPMethodRef: print_method_ref_cp_entry(const_pool, cp_entry); break;
        case CPClass: print_class_cp_entry(const_pool, cp_entry); break;
        case CPNameAndType: print_nametype_cp_entry(const_pool, cp_entry); break;
        default: printf("Unknown");
    }

    printf("\n");
}

JClass* jclass_load(const char* buffer) {
    size_t buffer_pos = 0;
    
    uint32_t magic_number = read_uint32(buffer, &buffer_pos);
    printf("magic number: %X\n", magic_number);

    uint16_t minor_version = read_uint16(buffer, &buffer_pos);
    printf("minor version: %d\n", minor_version);

    uint16_t major_version = read_uint16(buffer, &buffer_pos);
    printf("major version: %d\n", major_version);

    uint16_t cp_size = read_uint16(buffer, &buffer_pos) - 1;
    printf("constant pool size: %d\n", cp_size);

    size_t cp_obj_size = sizeof(CPool) + cp_size * sizeof(CPEntry);
    CPool* const_pool = malloc(cp_obj_size);
    if (const_pool == NULL) {
        fprintf(stderr, "constant pool allocation failed\n");
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

    for (int cp_entry_idx = 0;cp_entry_idx < cp_size;cp_entry_idx++) {
        print_cp_entry(cp_entry_idx, const_pool);
    }

    uint16_t access_flags = read_uint16(buffer, &buffer_pos);
    printf("access flags: %X\n", access_flags);

    uint16_t this_class_cp_entry_idx = read_uint16(buffer, &buffer_pos);
    char* class_name = get_class_name(this_class_cp_entry_idx, const_pool);
    printf("class name: %s\n", class_name);

    JClass* jclass = malloc(sizeof(JClass));
    if (jclass == NULL) {
        fprintf(stderr, "JClass allocation failed\n");
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

int main(int argc, char* argv[]) {
    const char* class_file_path = "classes/Service.class";

    long class_file_size = get_file_size(class_file_path);
    if (class_file_size == -1) {
        perror("failed to get file size: ");
        return EXIT_FAILURE;
    }

    printf("class file size %ld bytes\n", class_file_size);

    FILE* class_file = fopen(class_file_path, "rb");
    if (class_file == NULL) {
        fprintf(stderr, "failed to open class file\n");
        return EXIT_FAILURE;
    }

    char class_buffer[class_file_size];
    size_t blocks_read = fread(class_buffer, class_file_size, 1, class_file);
    if (blocks_read == 0) {
        fprintf(stderr, "failed to read class file\n");
        return EXIT_FAILURE;
    }

    JClass* jclass = jclass_load(class_buffer);
    if (jclass == NULL) {
        return EXIT_FAILURE;
    }

    printf("class signature: L%s;\n", jclass->name);

    jclass_free(jclass);

    int status = fclose(class_file);
    if (status != 0) {
        fprintf(stderr, "failed to close class file\n");
        return EXIT_FAILURE;
    }
    
    return EXIT_SUCCESS;
}