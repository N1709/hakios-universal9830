/* Read-only PC/LR/run-state sample. No suspension, stack contents or arguments. */
#include <mach/mach.h>
#include <mach/arm/thread_status.h>
#include <mach-o/dyld.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
int main(int argc,char **argv){
 if(argc!=2)return 2;
 char *end=NULL;long pid=strtol(argv[1],&end,10);
 if(!end || *end || pid<=1 || pid>1000000)return 2;
 dlopen("/System/Library/PrivateFrameworks/AppleMediaServices.framework/AppleMediaServices",RTLD_NOW);
 intptr_t slide=0;
 for(uint32_t i=0;i<_dyld_image_count();i++){
  const char *name=_dyld_get_image_name(i);
  if(name && strstr(name,"/AppleMediaServices.framework/AppleMediaServices")){slide=_dyld_get_image_vmaddr_slide(i);break;}
 }
 mach_port_t task=MACH_PORT_NULL;kern_return_t rc=task_for_pid(mach_task_self(),(int)pid,&task);
 if(rc){printf("task_for_pid_error=%d\n",rc);return 3;}
 thread_act_array_t threads=NULL;mach_msg_type_number_t count=0;
 rc=task_threads(task,&threads,&count);
 if(rc){printf("task_threads_error=%d\n",rc);mach_port_deallocate(mach_task_self(),task);return 4;}
 printf("pid=%ld threads=%u\n",pid,count);
 for(unsigned i=0;i<count;i++){
  if(i<64){
   arm_thread_state64_t state;mach_msg_type_number_t n=ARM_THREAD_STATE64_COUNT;
   struct thread_basic_info basic;mach_msg_type_number_t bn=THREAD_BASIC_INFO_COUNT;
   if(thread_get_state(threads[i],ARM_THREAD_STATE64,(thread_state_t)&state,&n)==KERN_SUCCESS &&
      thread_info(threads[i],THREAD_BASIC_INFO,(thread_info_t)&basic,&bn)==KERN_SUCCESS)
    printf("thread=%u state=%d pc=0x%llx lr=0x%llx\n",i,basic.run_state,
      (unsigned long long)(arm_thread_state64_get_pc(state)-slide),
      (unsigned long long)(arm_thread_state64_get_lr(state)-slide));
  }
  mach_port_deallocate(mach_task_self(),threads[i]);
 }
 vm_deallocate(mach_task_self(),(vm_address_t)threads,count*sizeof(*threads));
 mach_port_deallocate(mach_task_self(),task);return 0;
}
