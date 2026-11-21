#!/usr/bin/env python3
"""Compile the actual kernel adapter against narrow host IOKit mocks.
Does not open a real AppleKeyStore or touch device/system credentials.
"""
from pathlib import Path
import subprocess
import tempfile
import json
root = Path(__file__).resolve().parents[2]
src = (root / 'xnu/files/iokit/Kernel/MiniAppleKeyStore.cpp').read_text()
enum = src[src.index('enum {'):src.index('\n};', src.index('enum {')) + 3]
start = src.index('IOReturn\nAppleKeyStoreUserClient::softwareMethod(')
end = src.index('\nIOReturn\nAppleKeyStoreUserClient::externalMethod(', start)
method = src[start:end]
preamble = r'''
#include <CommonCrypto/CommonHMAC.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include HEADER
using IOReturn = int;
enum { kIOReturnSuccess, kIOReturnBadArgument, kIOReturnNotPermitted,
 kIOReturnNotPrivileged, kIOReturnNotReady, kIOReturnNoResources, kIOReturnUnsupported,
 kIOReturnNoSpace };
struct Descriptor {
 const void *bytes; size_t size;
 size_t getLength() { return size; }
 size_t readBytes(size_t off, void *out, size_t n) {
  if(off > size || n > size-off) return 0;
  memcpy(out,(const char*)bytes+off,n); return n;
 }
};
struct IOExternalMethodArguments {
 const uint64_t *scalarInput = nullptr; uint32_t scalarInputCount = 0;
 uint64_t *scalarOutput = nullptr; uint32_t scalarOutputCount = 0;
 Descriptor *structureInputDescriptor = nullptr;
 const void *structureInput = nullptr; size_t structureInputSize = 0;
 void *structureOutput = nullptr; size_t structureOutputSize = 0;
};
static bool privileged = true;
static void *kauth_cred_get() { return nullptr; }
static bool kauth_cred_issuser(void*) { return privileged; }
struct Crypto { void *ccsha256_di, *cchmac_fn; } crypto{(void*)1,(void*)1};
static Crypto *g_crypto_funcs = &crypto;
static const uint8_t bagUUID[16] = {0x72,0x65,0x64,0x77,0x6f,0x6f,0x64,0,0x6b,0x65,0x79,0x62,0x61,0x67,0,1};
static swkb_state gSoftBags[16]; static bool gSoftUsed[16];
static uint64_t softNow() { return 100; }
static void softRandom(void *out,size_t n) { memset(out,0x5a,n); }
static void softHmac(const void *key,size_t kn,const void *data,size_t dn,uint8_t out[32]) {
 CCHmac(kCCHmacAlgSHA256,key,kn,data,dn,out);
}
static bool softCopySecret(uint64_t address,uint64_t n,uint8_t out[SWKB_MAX_SECRET]) {
 if(n > SWKB_MAX_SECRET || (n && address < 4096)) return false;
 if(n) memcpy(out,(void*)(uintptr_t)address,(size_t)n);
 return true;
}
static IOReturn softResult(int r) {
 return !r?kIOReturnSuccess:r==SWKB_BACKOFF?kIOReturnNotReady:
 r==SWKB_BAD_RECORD?kIOReturnBadArgument:kIOReturnNotPermitted;
}
static size_t deviceState(uint8_t *out,const swkb_state *s) { *out=(uint8_t)swkb_flags(s); return 1; }
class AppleKeyStoreUserClient {
public:
 static IOReturn softwareMethod(uint32_t,IOExternalMethodArguments*);
 static IOReturn answer(IOExternalMethodArguments *a,const uint8_t *p,size_t n) {
  if(!a->structureOutput || a->structureOutputSize<n) return kIOReturnNoSpace;
  memcpy(a->structureOutput,p,n); a->structureOutputSize=n; return 0;
 }
};
'''.replace('HEADER', json.dumps(str(root / 'xnu/files/iokit/Kernel/SoftwareKeybag.h')))
test = r'''
static int call(unsigned op,const uint64_t *in,unsigned count,uint64_t *out=nullptr) {
 IOExternalMethodArguments a;
 a.scalarInput=in; a.scalarInputCount=count; a.scalarOutput=out; a.scalarOutputCount=out?1:0;
 return AppleKeyStoreUserClient::softwareMethod(op,&a);
}
static int change(unsigned bag,const char *old,const char *next,unsigned flags=0,uint64_t *result=nullptr) {
 uint64_t in[]={bag,flags,255,(uint64_t)old,strlen(old),(uint64_t)next,strlen(next),0,0,0,0},out;
 int r=call(15,in,11,&out); if(!r && result)*result=out; return r;
}
int main() {
 uint64_t create[]={0,(uint64_t)-1,0,0,0,0},bag=0,clone=0;
 assert(call(2,create,6,&bag)==0 && bag>0);
 assert(!memcmp(gSoftBags[0].record.uuid,bagUUID,sizeof(bagUUID)));
 assert(change((unsigned)bag,"","probe-A")==0);
 assert(call(8,&bag,1)==0 && gSoftBags[bag].locked);
 uint64_t bad[]={bag,(uint64_t)"wrong",5};
 assert(call(9,bad,3)==kIOReturnNotPermitted && gSoftBags[bag].locked);
 uint64_t malformed[]={bag,1,SWKB_MAX_SECRET+1};
 assert(call(9,malformed,3)==kIOReturnBadArgument);
 assert(call(9,malformed,2)==kIOReturnBadArgument);
 privileged=false;
 assert(change((unsigned)bag,"probe-A","probe-B")==kIOReturnNotPrivileged);
 uint64_t flags=0;
 assert(call(7,&bag,1,&flags)==0 && (flags&1));
 privileged=true;
 assert(change((unsigned)bag,"probe-A","probe-B",4,&clone)==0 && clone!=bag);
 assert(gSoftBags[0].record.has_secret==0); /* uncommitted clone cannot change system */
 assert(swkb_check(&gSoftBags[bag],softHmac,"probe-A",7,100)==0);
 assert(swkb_check(&gSoftBags[clone],softHmac,"probe-B",7,100)==0);
 swkb_record record;
 IOExternalMethodArguments save;
 save.scalarInput=&clone;save.scalarInputCount=1;
 save.structureOutput=&record;save.structureOutputSize=sizeof(record)-1;
 assert(AppleKeyStoreUserClient::softwareMethod(3,&save)==kIOReturnNoSpace);
 save.structureOutputSize=sizeof(record);
 assert(AppleKeyStoreUserClient::softwareMethod(3,&save)==0);
 uint64_t loaded=0;
 IOExternalMethodArguments load;
 load.structureInput=&record; load.structureInputSize=sizeof(record);
 load.scalarOutput=&loaded;load.scalarOutputCount=1;
 assert(AppleKeyStoreUserClient::softwareMethod(6,&load)==0);
 assert(gSoftBags[loaded].locked && !gSoftBags[loaded].first_unlock);
 uint64_t install[]={loaded,0,0,0};
 privileged=false;
 assert(call(5,install,4)==kIOReturnNotPrivileged && !gSoftBags[0].record.has_secret);
 privileged=true;
 assert(call(5,install,4)==0 && gSoftBags[0].locked && gSoftBags[0].record.has_secret);
 uint64_t epilogue[]={0,(uint64_t)"wrong",5};
 assert(call(0x69,epilogue,3)==kIOReturnNotPermitted && gSoftBags[0].locked);
 epilogue[1]=(uint64_t)"probe-B";epilogue[2]=7;
 assert(call(0x69,epilogue,3)==0 && gSoftBags[0].locked); /* completion cannot unlock by itself */
 assert(call(4,&loaded,1)==0 && !gSoftUsed[loaded]);
 assert(gSoftBags[0].record.has_secret); /* unload candidate does not remove system */
 record.rounds++;
 assert(AppleKeyStoreUserClient::softwareMethod(6,&load)==kIOReturnBadArgument);
 assert(call(2,nullptr,6,&loaded)==kIOReturnBadArgument);
 puts("PASS: actual adapter ABI, privilege/input guards, transaction clone, persistence and system installation");
}
'''
with tempfile.TemporaryDirectory() as d:
    path = Path(d)/'adapter.cpp'
    binary = Path(d)/'adapter-test'
    path.write_text(preamble + '\n' + enum + '\n' + method + '\n' + test)
    subprocess.run(['c++','-std=c++17','-O1','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined',str(path),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
