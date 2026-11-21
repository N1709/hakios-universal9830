/* Runtime method metadata only. Construct a suspended task; never resume it. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <mach-o/loader.h>
static void address(const char *name,const char *selector) {
    Method method=class_getInstanceMethod(objc_getClass(name),sel_registerName(selector));
    if(!method)return;
    IMP imp=method_getImplementation(method);Dl_info info;
    if(!dladdr((void *)imp,&info) || !info.dli_fbase)return;
    const struct mach_header_64 *header=info.dli_fbase;
    if(header->magic!=MH_MAGIC_64)return;
    const struct load_command *command=(const void *)(header+1);
    for(unsigned i=0;i<header->ncmds;i++) {
        if(command->cmd==LC_SEGMENT_64) {
            const struct segment_command_64 *segment=(const void *)command;
            if(!strcmp(segment->segname,"__TEXT")) {
                printf("%s %s unslid=0x%llx\n",name,selector,
                    (unsigned long long)((uintptr_t)imp-(uintptr_t)header+segment->vmaddr));
                return;
            }
        }
        command=(const void *)((const char *)command+command->cmdsize);
    }
}
static void dump(Class cls, BOOL filtered) {
    unsigned count=0; Method *methods=class_copyMethodList(cls,&count);
    for(unsigned i=0;i<count;i++) {
        const char *name=sel_getName(method_getName(methods[i]));
        if(filtered && !strcasestr(name,"finish") && !strcasestr(name,"complet") &&
           strcmp(name,"resume") && !strcasestr(name,"authenticateWithContext"))continue;
        printf("%s %s %s\n",class_getName(cls),name,method_getTypeEncoding(methods[i]));
    }
    free(methods);
}
int main(int argc, const char **argv) { @autoreleasepool {
    if(argc==2 && !strcmp(argv[1],"--addresses")) {
        dlopen("/System/Library/PrivateFrameworks/AppleAccount.framework/AppleAccount",RTLD_NOW);
        for(NSString *name in @[@"AALoginAccountRequest",@"AALoginDelegatesRequest",@"AARegisterRequest",@"AASetupAssistantSetupDelegatesRequest"])
            address(name.UTF8String,"urlRequest");
        address("AARequest","_handleDataTaskCompletionWithData:response:error:");
        address("AARequest","bodyDictionary");
        address("AAiCloudLoginAccountRequester","loginWithAccount:forDelegates:completion:");
        address("NSMutableURLRequest","aa_addAltDSIDAndRepairStateWithAccount:");
        address("AADeviceProvisioningSession","addProvisioningInfoToURLRequest:sendEmptyValues:");
        address("AADeviceProvisioningSession","deviceProvisioningInfo");
        address("AAResponse","initWithHTTPResponse:data:");
        address("AALoginAccountResponse","initWithHTTPResponse:data:");
        address("AALoginAccountResponse","statusMessage");
        address("NSMutableURLRequest","aa_addBasicAuthorizationHeaderWithAccount:preferUsingPassword:");
        address("AARequest","urlRequest");
        address("NSMutableURLRequest","aa_addDeviceProvisioningInfoHeadersWithAccount:");
        address("AARequest","performSignedRequestWithHandler:");
        address("AARequest","performRequestForDevice:anisetteDataProvider:withHandler:");
        address("AARequest","performRequestWithSession:withHandler:");
        address("AAAbsintheSigner","signatureForData:completion:");
        address("AAAbsintheContext","cao1NI5PNJBn:error:");
        address("AAURLSessionContext","_additionalHeadersForRequest:completion:");
        address("AAURLSessionContext","_additionalAbsintheHeadersForData:completion:");
        return 0;
    }
    if(argc==2 && !strcmp(argv[1],"--appleaccount")) {
        dlopen("/System/Library/PrivateFrameworks/AppleAccount.framework/AppleAccount",RTLD_NOW);
        unsigned count=0;Class *classes=objc_copyClassList(&count);
        for(unsigned i=0;i<count;i++) {
            const char *name=class_getName(classes[i]);
            if(!strncmp(name,"AA",2) && (strstr(name,"Response") || strstr(name,"Request") || strstr(name,"AccountManager") || strstr(name,"Authenticate") || strstr(name,"SigningSession") || strstr(name,"Absinthe") || strstr(name,"Provisioning") || strstr(name,"Feature") || strstr(name,"URL") || strstr(name,"Login") || strstr(name,"SignIn"))) {
                dump(classes[i],NO);
                if(strstr(name,"SigningSession") || strstr(name,"Absinthe") || strstr(name,"Feature") || strstr(name,"URLConfiguration"))dump(object_getClass(classes[i]),NO);
            }
        }
        free(classes);return 0;
    }
    NSURLSession *session=[NSURLSession sessionWithConfiguration:NSURLSessionConfiguration.ephemeralSessionConfiguration];
    NSURLSessionDataTask *task=[session dataTaskWithURL:[NSURL URLWithString:@"https://example.invalid/"]];
    for(Class cls=task.class;cls && cls!=NSObject.class;cls=class_getSuperclass(cls))dump(cls,YES);
    [task cancel]; [session invalidateAndCancel];
    dlopen("/System/Library/PrivateFrameworks/AuthKit.framework/AuthKit",RTLD_NOW);
    Class controller=objc_getClass("AKAppleIDAuthenticationController");
    if(controller)dump(controller,YES);
    return 0;
}}
