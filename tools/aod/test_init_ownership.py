#!/usr/bin/env python3
"""Check the actual hooked initializer obeys Objective-C init ownership."""
from pathlib import Path
import re
import subprocess
import tempfile
repo=Path(__file__).resolve().parents[2]
s=(repo/'tools/aod/alwayson.m').read_text()
typedef=re.search(r'typedef id \(\*RWEngineInit\).*?;',s).group()
start=s.index('static id engineInitialized(')
brace=s.index('{',start);end=brace+1;depth=1
while depth:
    depth+=(s[end]=='{')-(s[end]=='}');end+=1
hook=s[start:end]
fixture='''
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
static unsigned deaths;
@interface AuditEngine:NSObject
@property BOOL pseudo;
- (id)initWithDelegate:(id)d platformProvider:(id)p osInterfaceProvider:(id)o inactiveBudgetPolicy:(id)b;
@end
@implementation AuditEngine
- (id)initWithDelegate:(id)d platformProvider:(id)p osInterfaceProvider:(id)o inactiveBudgetPolicy:(id)b {
    (void)d;(void)p;(void)o;(void)b;return [super init];
}
- (void)dealloc {deaths++;}
@end
static BOOL setBool(id obj,const char *name,BOOL value){(void)name;[(AuditEngine*)obj setPseudo:value];return YES;}
'''
main='''
int main(){@autoreleasepool{
    Method m=class_getInstanceMethod(AuditEngine.class,@selector(initWithDelegate:platformProvider:osInterfaceProvider:inactiveBudgetPolicy:));
    engineInitOriginal=(RWEngineInit)method_setImplementation(m,(IMP)engineInitialized);
    for(unsigned i=0;i<1000;i++){@autoreleasepool{
        AuditEngine *e=[[AuditEngine alloc]initWithDelegate:nil platformProvider:nil osInterfaceProvider:nil inactiveBudgetPolicy:nil];
        assert(e.pseudo);
    }}
    assert(deaths==1000);puts("PASS: actual engine initializer retains init ownership without leaked objects");
}return 0;}
'''
with tempfile.TemporaryDirectory(prefix='aod-ownership-') as d:
    p=Path(d);(p/'test.m').write_text(fixture+typedef+'\nstatic RWEngineInit engineInitOriginal;\n'+hook+main)
    subprocess.run(['xcrun','clang','-fobjc-arc','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-framework','Foundation',str(p/'test.m'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
