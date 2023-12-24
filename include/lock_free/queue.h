#ifndef __LOCK_FREE_QUEUE_H__
#define __LOCK_FREE_QUEUE_H__

#include <common.h>      // BEGIN, END DECLS
#include "liblfds711.h"  // Lock-free structures

#define INIT_QUEUE_SIZE             128
#define ADDITIONAL_LIST_ELEMENTS    4

typedef struct {
    struct lfds711_queue_umm_state   queue;
    struct lfds711_queue_umm_element dummy_element;
    struct lfds711_queue_umm_element elements[INIT_QUEUE_SIZE];

    // struct lfds711_prng_st_state    *psts;
    // struct lfds711_freelist_element
    //      volatile (*elimination_array)[LFDS711_FREELIST_ELIMINATION_ARRAY_ELEMENT_SIZE_IN_FREELIST_ELEMENTS];

    struct lfds711_freelist_state    freelist;
    struct lfds711_freelist_element  list_e[INIT_QUEUE_SIZE];
} Queue;

__BEGIN_DECLS

static inline void queue_init_barrier(void)
{
    LFDS711_MISC_MAKE_VALID_ON_CURRENT_LOGICAL_CORE_INITS_COMPLETED_BEFORE_NOW_ON_ANY_OTHER_LOGICAL_CORE;
}

errval_t queue_init(Queue* queue);
void queue_destroy(Queue* queue);
void enqueue(Queue* queue, void* data);
errval_t dequeue(Queue* queue, void** ret_data);

__END_DECLS

#endif // __LOCK_FREE_QUEUE_H__