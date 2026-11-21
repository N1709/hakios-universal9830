/* Export only this process's loaded framework STATIC text and symbol/selector
 * names. Never reads daemon memory, account state, tokens or writable data. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <mach-o/loader.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
static NSString *hex(uintptr_t value){return [NSString stringWithFormat:@"0x%llx",(unsigned long long)value];}
int main(int argc,const char **argv) { @autoreleasepool {
    if(argc!=2 && argc!=3)return 2;
    dlopen("/System/Library/PrivateFrameworks/AppleAccount.framework/AppleAccount",RTLD_NOW);
    Class cls=objc_getClass("AASignInFlowController");Method probe=class_getInstanceMethod(cls,sel_registerName("_onqueue_createNewAppleAccountWithAuthResults:accountCreationError:"));
    Dl_info info;if(!probe || !dladdr((void *)method_getImplementation(probe),&info))return 3;
    const struct mach_header_64 *header=info.dli_fbase;if(header->magic!=MH_MAGIC_64)return 4;
    const struct load_command *cmd=(const void *)(header+1);const struct segment_command_64 *text=NULL;
    for(unsigned i=0;i<header->ncmds;i++){if(cmd->cmd==LC_SEGMENT_64 && !strcmp(((const struct segment_command_64 *)cmd)->segname,"__TEXT"))text=(const void *)cmd;cmd=(const void *)((const char *)cmd+cmd->cmdsize);}
    if(!text || text->vmsize>64*1024*1024)return 5;
    intptr_t slide=(intptr_t)header-text->vmaddr;
    NSMutableDictionary *selectors=[NSMutableDictionary dictionary],*symbols=[NSMutableDictionary dictionary],*methods=[NSMutableDictionary dictionary];
    cmd=(const void *)(header+1);
    for(unsigned i=0;i<header->ncmds;i++) {
        if(cmd->cmd==LC_SEGMENT_64) {
            const struct segment_command_64 *segment=(const void *)cmd;const struct section_64 *sections=(const void *)(segment+1);
            for(unsigned j=0;j<segment->nsects;j++) {
                const struct section_64 *section=&sections[j];
                BOOL sel=!strcmp(section->sectname,"__objc_selrefs"),got=!strcmp(section->sectname,"__got");
                if((sel || got) && section->size<=1024*1024) {
                    const uintptr_t *slots=(const void *)(section->addr+slide);
                    for(NSUInteger k=0;k<section->size/sizeof(uintptr_t);k++) {
                        NSString *name=nil;
                        if(sel){const char *value=sel_getName((SEL)slots[k]);if(value)name=[NSString stringWithUTF8String:value];}
                        else {Dl_info target;if(dladdr((void *)slots[k],&target) && target.dli_sname)name=[NSString stringWithUTF8String:target.dli_sname];}
                        if(name.length && name.length<1024)(sel?selectors:symbols)[hex((uintptr_t)&slots[k])]=name;
                    }
                }
            }
        }
        cmd=(const void *)((const char *)cmd+cmd->cmdsize);
    }
    unsigned count=0;Method *list=class_copyMethodList(cls,&count);
    for(unsigned i=0;i<count;i++)methods[NSStringFromSelector(method_getName(list[i]))]=hex((uintptr_t)method_getImplementation(list[i]));free(list);
    for(NSArray *pair in @[@[@"ACAccount",@"aa_password"],@[@"NSMutableURLRequest",@"aa_addBasicAuthorizationHeaderWithAccount:preferUsingPassword:"],@[@"AALoginAccountRequest",@"urlRequest"]]) {
        Method extra=class_getInstanceMethod(objc_getClass([pair[0] UTF8String]),NSSelectorFromString(pair[1]));
        if(extra) {
            uintptr_t address=(uintptr_t)method_getImplementation(extra);
            if(address>=(uintptr_t)header && address<(uintptr_t)header+text->vmsize)
                methods[[pair componentsJoinedByString:@" "]]=hex(address);
        }
    }
    NSString *directory=[NSString stringWithUTF8String:argv[1]];
    if(![NSFileManager.defaultManager createDirectoryAtPath:directory withIntermediateDirectories:NO attributes:@{NSFilePosixPermissions:@0700} error:NULL])return 6;
    NSData *bytes=[NSData dataWithBytes:(void *)(text->vmaddr+slide) length:text->vmsize];
    if(![bytes writeToFile:[directory stringByAppendingPathComponent:@"AppleAccount.text"] atomically:YES])return 7;
    NSMutableDictionary *resolved=[NSMutableDictionary dictionary];
    if(argc==3) {
        NSData *input=[NSData dataWithContentsOfFile:[NSString stringWithUTF8String:argv[2]]];
        id request=input?[NSJSONSerialization JSONObjectWithData:input options:0 error:NULL]:nil;
        if(![request isKindOfClass:NSDictionary.class] || ![request[@"text_base"] isEqual:hex(text->vmaddr+slide)])return 9;
        NSArray *slots=request[@"slots"];if(![slots isKindOfClass:NSArray.class] || slots.count>4096)return 10;
        NSArray *known=@[@"AKAuthenticationUsernameKey",@"AKAuthenticationPasswordKey",@"AKAuthenticationRawPasswordKey",@"AKAuthenticationDSIDKey",@"AKAuthenticationAlternateDSIDKey",@"AKAuthenticationIDMSTokenKey",@"AKAuthenticationIsHSA2Key",@"AKAuthenticationShortLivedTokenKey"];
        for(id slot in slots) {
            if(![slot isKindOfClass:NSString.class] || [slot length]>20)continue;
            uintptr_t address=(uintptr_t)strtoull([slot UTF8String],NULL,16),pointer=0;mach_vm_size_t readSize=0;
            if(mach_vm_read_overwrite(mach_task_self(),address,sizeof(pointer),(mach_vm_address_t)&pointer,&readSize)!=KERN_SUCCESS || readSize!=sizeof(pointer))continue;
            for(NSString *name in known)if(pointer==(uintptr_t)dlsym(RTLD_DEFAULT,name.UTF8String))resolved[slot]=name;
        }
    }
    NSDictionary *metadata=@{@"loaded_text_address":hex(text->vmaddr+slide),@"text_size":@(text->vmsize),@"resolved_import_slots":resolved,@"selector_slots":selectors,@"symbol_slots":symbols,@"methods":methods,@"authentication_called":@NO};
    NSData *json=[NSJSONSerialization dataWithJSONObject:metadata options:0 error:NULL];
    if(![json writeToFile:[directory stringByAppendingPathComponent:@"metadata.json"] atomically:YES])return 8;
    puts("Exported framework static text and selector/symbol names only; no authentication or account data");return 0;
}}
