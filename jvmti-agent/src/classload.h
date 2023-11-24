#ifndef _CLASSLOAD_H_
#define _CLASSLOAD_H_

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
    char* name;
    CPool* const_pool;
} JClass;

JClass* jclass_load(const uint8_t* buffer);

void jclass_free(JClass* jclass);

#endif