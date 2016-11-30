#ifndef _BLOCK_H_
#define _BLOCK_H_

#include "includes.h"

/* Representing an extent on disk */
struct extent {

    /* Start block */
    uint64_t ex_start;

    /* Count of blocks */
    uint64_t ex_count;

    /* Next extent on the device */
    struct extent *ex_next;
};

#endif
