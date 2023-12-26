#ifndef __LOCK_FREE_H__
#define __LOCK_FREE_H__

#define CORES_SYNC_BARRIER  LFDS711_MISC_MAKE_VALID_ON_CURRENT_LOGICAL_CORE_INITS_COMPLETED_BEFORE_NOW_ON_ANY_OTHER_LOGICAL_CORE
#define ATOMIC_ISOLATION    LFDS711_PAL_ATOMIC_ISOLATION_IN_BYTES

typedef int (*list_key_compare)(void const *new_key, void const *existing_key);

#endif // __LOCK_FREE_H__
