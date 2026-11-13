#include <stdint.h>
#include <stdlib.h>
static void *saved;
void publish(void *handle) { saved = handle; }
void *get_handle(void) { return saved; }
void *allocate(uint64_t size) { return malloc(size); }
void release(void *memory) { free(memory); }
