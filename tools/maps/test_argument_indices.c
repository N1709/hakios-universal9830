#include "../../userland/src/metal_argument_indices.h"
#include <assert.h>
#include <stdio.h>
int main(void){
 uint64_t seen=0;unsigned length=0;
 assert(rw_argument_index_add(&seen,&length,0)&&length==8);
 assert(rw_argument_index_add(&seen,&length,5)&&length==48);
 assert(rw_argument_index_add(&seen,&length,2)&&length==48);
 uint64_t before=seen;assert(!rw_argument_index_add(&seen,&length,2)&&seen==before&&length==48);
 assert(!rw_argument_index_add(&seen,&length,64)&&seen==before);
 assert(!rw_argument_index_add(&seen,&length,UINT64_MAX)&&seen==before);
 assert(rw_argument_index_add(&seen,&length,63)&&length==512);
 puts("PASS: sparse, unordered and duplicate argument IDs, bounds and encoded span");
}
