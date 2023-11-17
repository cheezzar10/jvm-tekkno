#ifndef _HASHMAP_H_
#define _HASHMAP_H_

#include <pthread.h>

typedef uint32_t HashFn(const char* key, size_t capacity);

typedef struct {
    size_t capacity;
    size_t size;
    size_t reallocation_limit;
    HashFn* hash_fn;
    pthread_mutex_t* mutex;
    void* buckets;
} HashMap;

HashMap* hash_map_new(size_t capacity, HashFn* hash_fn);

void* hash_map_get(const HashMap* hash_map, const char* key);

bool hash_map_put(HashMap* hash_map, const char* key, void* value);

void hash_map_free(HashMap* hash_map);

#endif
