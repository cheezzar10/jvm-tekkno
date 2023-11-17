#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "hashmap.h"

typedef struct HashMapEntry {
    char* key;
    void* value;
    struct HashMapEntry* next_entry;
} HashMapEntry;

typedef struct {
    HashMapEntry* entries;
} HashMapBucket;

static uint32_t hash(const char* key, size_t capacity) {
    uint32_t h = 0;
    size_t key_length = strlen(key);
    for (size_t idx = 0;idx < key_length;idx++) {
        h = (h * 31 + key[idx]) % capacity;
    }
    return h;
}

HashMap* hash_map_new(size_t capacity, HashFn* hash_fn) {
    HashMap* hash_map = malloc(sizeof(HashMap));
    if (hash_map == NULL) {
        return NULL;
    }

    HashMapBucket* new_buckets = calloc(capacity, sizeof(HashMapBucket));
    if (new_buckets == NULL) {
        return NULL;
    }

    hash_map->capacity = capacity;
    hash_map->size = 0;
    hash_map->buckets = new_buckets;
    hash_map->reallocation_limit = capacity * 0.75;

    if (hash_fn != NULL) {
        hash_map->hash_fn = hash_fn;
    } else {
        hash_map->hash_fn = hash;
    }

    hash_map->mutex = calloc(1, sizeof(pthread_mutex_t));
    pthread_mutex_init(hash_map->mutex, NULL);

    return hash_map;
}

void* hash_map_get(const HashMap* hash_map, const char* key) {
    pthread_mutex_lock(hash_map->mutex);

    void* result = NULL;

    if (hash_map->size > 0) {
        uint32_t hash = hash_map->hash_fn(key, hash_map->capacity);

        HashMapBucket* bucket = ((HashMapBucket*)hash_map->buckets) + hash;
        if (bucket->entries != NULL) {
            size_t key_length = strlen(key);
            
            HashMapEntry* head_entry = bucket->entries;
            if (strncmp(key, head_entry->key, key_length) == 0) {
                result = head_entry->value;
            } else {
                HashMapEntry* current_entry = head_entry;
                while (current_entry->next_entry != NULL) {
                    current_entry = current_entry->next_entry;

                    if (strncmp(key, current_entry->key, key_length) == 0) {
                        result = current_entry->value;
                        break;
                    }
                }
            }
        }
    }

    pthread_mutex_unlock(hash_map->mutex);

    return result;
}

static bool hash_map_entry_set_key(HashMapEntry* entry, const char* key, size_t key_length) {
    entry->key = malloc(key_length + 1);
    if (entry->key == NULL) {
        return false;
    }

    memset(entry->key, 0, key_length + 1);
    strncpy(entry->key, key, key_length);

    return true;
}

static void hash_map_bucket_add_entry(HashMapBucket* bucket, HashMapEntry* entry) {
    HashMapEntry* current_entry = bucket->entries;
    
    if (current_entry == NULL) {
        bucket->entries = entry;
    } else {
        while (current_entry->next_entry != NULL) {
            current_entry = current_entry->next_entry;
        }

        current_entry->next_entry = entry;
    }
}

static void hash_map_entry_free(HashMapEntry* entry) {
    free(entry->key);
    free(entry);
}

static bool hash_map_reallocate_and_put(HashMap* hash_map, size_t new_capacity, const char* key, void* value) {
    HashMapBucket* new_buckets = calloc(new_capacity, sizeof(HashMapBucket));
    if (new_buckets == NULL) {
        return false;
    }

    for (size_t bucket_index = 0;bucket_index < hash_map->capacity;bucket_index++) {
        HashMapBucket* current_bucket = ((HashMapBucket*)hash_map->buckets) + bucket_index;

        HashMapEntry* current_entry = current_bucket->entries;
        while (current_entry != NULL) {
            HashMapEntry* next_entry = current_entry->next_entry;

            current_entry->next_entry = NULL;

            uint32_t hash = hash_map->hash_fn(current_entry->key, new_capacity);
            HashMapBucket* entry_bucket = new_buckets + hash;
            hash_map_bucket_add_entry(entry_bucket, current_entry);

            current_entry = next_entry;
        }
    }

    HashMapBucket* old_buckets = hash_map->buckets;
    
    hash_map->buckets = new_buckets;
    hash_map->capacity = new_capacity;
    hash_map->reallocation_limit = 0.75 * new_capacity;

    free(old_buckets);

    return true;
}

bool hash_map_put(HashMap* hash_map, const char* key, void* value) {
    pthread_mutex_lock(hash_map->mutex);
    
    bool put_success = true;

    for (;;) {
        uint32_t hash = hash_map->hash_fn(key, hash_map->capacity);
        HashMapBucket* bucket = ((HashMapBucket*)hash_map->buckets) + hash;

        size_t key_length = strlen(key);

        HashMapEntry* head_entry = bucket->entries;
        if (head_entry == NULL) {
            if (hash_map->size == hash_map->reallocation_limit) {
                bool reallocation_success = hash_map_reallocate_and_put(hash_map, hash_map->capacity * 2, key, value);
                if (!reallocation_success) {
                    put_success = false;
                    break;
                }
                
                continue; // reallocation completed, retrying put
            }

            HashMapEntry* new_head_entry = malloc(sizeof(HashMapEntry));
            if (new_head_entry == NULL) {
                put_success = false;
                break;
            }

            memset(new_head_entry, 0, sizeof(HashMapEntry));
            bool set_key_success = hash_map_entry_set_key(new_head_entry, key, key_length);
            if (!set_key_success) {
                return put_success = false;
                break;
            }
            
            new_head_entry->value = value;

            bucket->entries = new_head_entry;

            hash_map->size += 1;

            break; // bucket head entry added
        } else if (strncmp(key, head_entry->key, key_length) == 0) {
            head_entry->value = value;

            break; // bucket head entry updated
        } else {
            HashMapEntry* current_entry = head_entry;
            while (current_entry->next_entry != NULL) {
                current_entry = current_entry->next_entry;

                if (strncmp(key, current_entry->key, key_length) == 0) {
                    current_entry->value = value;
                    break; // bucket entry updated
                }
            }

            if (hash_map->size == hash_map->reallocation_limit) {
                bool reallocation_success = hash_map_reallocate_and_put(hash_map, hash_map->capacity * 2, key, value);
                if (!reallocation_success) {
                    put_success = false;
                    break;
                }
                
                continue; // reallocation completed, retrying put
            }

            HashMapEntry* new_entry = malloc(sizeof(HashMapEntry));
            if (new_entry == NULL) {
                put_success = false;
                break;
            }

            memset(new_entry, 0, sizeof(HashMapEntry));

            current_entry->next_entry = new_entry;

            bool set_key_success = hash_map_entry_set_key(new_entry, key, key_length);
            if (!set_key_success) {
                put_success = false;
                break;
            }
            
            new_entry->value = value;

            hash_map->size += 1;

            break; // bucket tail entry added
        }
    }

    pthread_mutex_unlock(hash_map->mutex);

    return put_success;
}

void hash_map_free(HashMap* hash_map) {
    for (size_t bucket_index = 0;bucket_index < hash_map->capacity;bucket_index++) {
        HashMapBucket* current_bucket = ((HashMapBucket*)hash_map->buckets) + bucket_index;

        HashMapEntry* current_entry = current_bucket->entries;
        while (current_entry != NULL) {
            HashMapEntry* next_entry = current_entry->next_entry;
            current_entry->next_entry = NULL;

            hash_map_entry_free(current_entry);

            current_entry = next_entry;
        }

        current_bucket->entries = NULL;
    }

    free(hash_map->buckets);

    pthread_mutex_destroy(hash_map->mutex);
    free(hash_map->mutex);

    free(hash_map);
}
