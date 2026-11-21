#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <dlfcn.h>
#import <objc/message.h>
int main(void){@autoreleasepool{
 id<MTLDevice>d=MTLCreateSystemDefaultDevice();if(!d){fprintf(stderr,"No Metal device\n");return 1;}
 MTLArgumentDescriptor*a=[MTLArgumentDescriptor argumentDescriptor];a.index=0;a.dataType=MTLDataTypePointer;
 MTLArgumentDescriptor*b=[MTLArgumentDescriptor argumentDescriptor];b.index=5;b.dataType=MTLDataTypeTexture;b.textureType=MTLTextureType2D;
 id<MTLArgumentEncoder>e=[d newArgumentEncoderWithArguments:@[b,a]];
 if(!e || e.encodedLength!=48){fprintf(stderr,"Sparse encoder wrong length: %lu\n",(unsigned long)e.encodedLength);return 1;}
 id<MTLBuffer>buffer=[d newBufferWithLength:48 options:0];[e setArgumentBuffer:buffer offset:0];
 MTLTextureDescriptor*td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
 id<MTLTexture>texture=[d newTextureWithDescriptor:td];if(!texture)return 1;
 [e setTexture:texture atIndex:5];
 void *lib=dlopen("/var/jb/Library/Extensions/RedwoodMapsMetal.bundle/libredwoodair.dylib",RTLD_NOW);
 unsigned(*members)(const void*,size_t)=lib?(void*)dlsym(lib,"rwa_argbuf_encoded_members"):NULL;
 const void *raw=((void*(*)(id,SEL))objc_msgSend)(buffer,sel_registerName("mesaBuffer"));
 if(!members || members(raw,0)!=1){fprintf(stderr,"Sparse ID was not recorded\n");return 1;}
 [e setTexture:texture atIndex:1];
 if(members(raw,0)!=1){fprintf(stderr,"Undeclared hole was encoded\n");return 1;}
 b.index=0;if([d newArgumentEncoderWithArguments:@[a,b]]){fprintf(stderr,"Duplicate ID accepted\n");return 1;}
 puts("PASS: native sparse argument encoder, unordered IDs, 48-byte span, sparse setter, undeclared-hole and duplicate refusal");return 0;
}}
