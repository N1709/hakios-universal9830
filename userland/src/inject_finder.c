/* Local Finder compatibility for the unprotected research port. This library
 * is loaded into lockdownd only; account/activation daemons retain real state.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

extern CFStringRef MAEGetActivationStateWithError(CFErrorRef *error);
#define DISABLED "/var/mobile/Library/Preferences/org.jesvi.finder-local.disabled"
#define TOKEN_PATH "/var/root/Library/Lockdown/jesvi-local-documents.token"
static const char token_prefix[] = "Jesvi local documents v1; no escrow keys:";
static int (*keybag_lock_state)(CFDictionaryRef);
static CFDataRef document_token;
static pthread_mutex_t token_lock = PTHREAD_MUTEX_INITIALIZER;

static bool local_mode_enabled(void)
{
    const char *name = getprogname();
    return name && !strcmp(name,"lockdownd") && access(DISABLED,F_OK)!=0 && errno==ENOENT;
}

static bool unprotected_documents(void)
{
    struct statfs fs; char marker[32]; size_t length=sizeof(marker);
    return local_mode_enabled() && keybag_lock_state && keybag_lock_state(NULL)==3 &&
        statfs("/private/var",&fs)==0 && !strcmp(fs.f_fstypename,"hfs") && !(fs.f_flags&MNT_CPROTECT) &&
        sysctlbyname("kern.redwood.usb.state",marker,&length,NULL,0)==0;
}

/* An opaque local marker, not an encrypted keybag or authentication replacement.
 * Native paired TLS and per-app folder checks remain mandatory. Never replace
 * real native escrow data. Root-only persistence keeps existing pairings usable
 * after reboot. Lazy loading handles the first boot before lockdown makes its
 * data directory; no files containing actual credentials are read or changed.
 */
static CFDataRef copy_document_token(void)
{
    pthread_mutex_lock(&token_lock);
    if (!document_token) {
        uint8_t bytes[sizeof(token_prefix)-1+32]; bool valid=false;
        int fd=open(TOKEN_PATH,O_RDONLY|O_NOFOLLOW);
        if(fd>=0){
            struct stat st;
            valid=fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_uid==0 &&
                !(st.st_mode&077) && st.st_size==sizeof(bytes) && read(fd,bytes,sizeof(bytes))==sizeof(bytes) &&
                !memcmp(bytes,token_prefix,sizeof(token_prefix)-1);
            close(fd);
        }else if(errno==ENOENT){
            fd=open(TOKEN_PATH,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
            if(fd>=0){
                memcpy(bytes,token_prefix,sizeof(token_prefix)-1);
                arc4random_buf(bytes+sizeof(token_prefix)-1,32);
                valid=write(fd,bytes,sizeof(bytes))==sizeof(bytes) && fsync(fd)==0;
                close(fd); if(!valid)unlink(TOKEN_PATH);
            }
        }
        if(valid)document_token=CFDataCreate(NULL,bytes,sizeof(bytes));
    }
    CFDataRef result=document_token?CFRetain(document_token):NULL;
    pthread_mutex_unlock(&token_lock);
    return result;
}

/* The desktop document helper requires escrow metadata even on this port's
 * no-passcode, non-content-protected HFS volume. Normalize only our own marker
 * for document/notification services; reject other uses before keybag processing. No keybag/crypto API is made to succeed,
 * and backup/encryption service requests are deliberately excluded.
 */
static CFPropertyListRef normalize_document_request(CFAllocatorRef allocator,CFPropertyListRef plist)
{
    if(!plist || CFGetTypeID(plist)!=CFDictionaryGetTypeID())return plist;
    CFDictionaryRef dict=(CFDictionaryRef)plist;
    CFTypeRef request=CFDictionaryGetValue(dict,CFSTR("Request"));
    CFTypeRef service=CFDictionaryGetValue(dict,CFSTR("Service"));
    CFTypeRef escrow=CFDictionaryGetValue(dict,CFSTR("EscrowBag"));
    if(!request || !service || !escrow || !CFEqual(request,CFSTR("StartService")) ||
        !unprotected_documents())return plist;
    CFDataRef token=copy_document_token(); bool match=token && CFEqual(escrow,token);
    if(token)CFRelease(token); if(!match)return plist;
    CFMutableDictionaryRef copy=CFDictionaryCreateMutableCopy(allocator,0,dict);
    if(!copy)return plist;
    if(CFEqual(service,CFSTR("com.apple.mobile.house_arrest")) ||
       CFEqual(service,CFSTR("com.apple.mobile.notification_proxy"))) {
        CFDictionaryRemoveValue(copy,CFSTR("EscrowBag"));
    } else {
        /* Reject unsupported escrow use before native keybag processing. An
         * EscrowLocked response makes MobileDevice discard the app-document
         * marker globally, breaking a subsequent unrelated document request.
         * A reserved nonexistent service produces native InvalidService;
         * no backup/crypto capability or successful service is fabricated. */
        CFDictionarySetValue(copy,CFSTR("Service"),CFSTR("org.jesvi.unsupported-local-escrow"));
        CFDictionaryRemoveValue(copy,CFSTR("EscrowBag"));
    }
    CFRelease(plist);
    return copy;
}

static CFDataRef finder_plist_output(CFAllocatorRef allocator,CFPropertyListRef plist,
    CFPropertyListFormat format,CFOptionFlags options,CFErrorRef *error)
{
    if(plist && CFGetTypeID(plist)==CFDictionaryGetTypeID()){
        CFDictionaryRef dict=(CFDictionaryRef)plist;
        CFTypeRef request=CFDictionaryGetValue(dict,CFSTR("Request"));
        CFTypeRef result=CFDictionaryGetValue(dict,CFSTR("Result"));
        if(request && CFEqual(request,CFSTR("Pair")) &&
            (!result || CFEqual(result,CFSTR("Success")) || CFEqual(result,kCFBooleanTrue)) &&
            !CFDictionaryGetValue(dict,CFSTR("Error")) && !CFDictionaryGetValue(dict,CFSTR("PairRecord")) &&
            !CFDictionaryGetValue(dict,CFSTR("EscrowBag")) && unprotected_documents()){
            CFDataRef token=copy_document_token();
            CFMutableDictionaryRef copy=token?CFDictionaryCreateMutableCopy(allocator,0,dict):NULL;
            if(copy){
                CFDictionarySetValue(copy,CFSTR("EscrowBag"),token); CFRelease(token);
                CFDataRef data=CFPropertyListCreateData(allocator,copy,format,options,error);
                CFRelease(copy); return data;
            }
            if(token)CFRelease(token);
        }
    }
    return CFPropertyListCreateData(allocator,plist,format,options,error);
}

static CFPropertyListRef finder_plist_data(CFAllocatorRef allocator,CFDataRef data,CFOptionFlags options,
    CFPropertyListFormat *format,CFErrorRef *error)
{
    return normalize_document_request(allocator,CFPropertyListCreateWithData(allocator,data,options,format,error));
}
static CFPropertyListRef finder_plist_stream(CFAllocatorRef allocator,CFReadStreamRef stream,CFIndex length,
    CFOptionFlags options,CFPropertyListFormat *format,CFErrorRef *error)
{
    return normalize_document_request(allocator,CFPropertyListCreateWithStream(allocator,stream,length,options,format,error));
}

static CFStringRef finder_activation_state(CFErrorRef *error)
{
    CFErrorRef native_error=NULL;
    CFStringRef state=MAEGetActivationStateWithError(&native_error);
    if(local_mode_enabled() && state && !native_error && CFEqual(state,CFSTR("Unactivated"))){
        CFRelease(state); if(error)*error=NULL;
        /* FactoryActivated skips Trust in native lockdown; use Activated to
         * preserve ordinary trust/paired-session checks for the local mode. */
        return CFRetain(CFSTR("Activated"));
    }
    if(error)*error=native_error; else if(native_error)CFRelease(native_error);
    return state;
}

__attribute__((used,section("__DATA,__interpose"))) static const struct {
    const void *replacement; const void *original;
} finder_interpose[]={
    {(const void *)finder_activation_state,(const void *)MAEGetActivationStateWithError},
    {(const void *)finder_plist_data,(const void *)CFPropertyListCreateWithData},
    {(const void *)finder_plist_stream,(const void *)CFPropertyListCreateWithStream},
    {(const void *)finder_plist_output,(const void *)CFPropertyListCreateData},
};

__attribute__((constructor)) static void finder_init(void)
{
    unsetenv("DYLD_INSERT_LIBRARIES");
    if(!getprogname() || strcmp(getprogname(),"lockdownd"))return;
    keybag_lock_state=dlsym(RTLD_DEFAULT,"MKBGetDeviceLockState");
    if(!keybag_lock_state){
        void *h=dlopen("/System/Library/PrivateFrameworks/MobileKeyBag.framework/MobileKeyBag",RTLD_LAZY);
        if(h)keybag_lock_state=dlsym(h,"MKBGetDeviceLockState");
    }
    if(unprotected_documents()){CFDataRef token=copy_document_token();if(token)CFRelease(token);}
    const char text[]="redwood: local Finder compatibility loaded; native Apple activation and iCloud unchanged\n";
    int fd=open("/dev/console",O_WRONLY);if(fd>=0){(void)write(fd,text,sizeof(text)-1);close(fd);}
}
