// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

// macOS-only interoperability probe. Not part of the Linux receiver.
// ABI facts: https://pyatv.dev/documentation/protocols/#reference-decoding
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
  @autoreleasepool {
    if (argc != 4 || (strcmp(argv[1], "encode") && strcmp(argv[1], "decode"))) return 2;
    void* framework = dlopen("/System/Library/PrivateFrameworks/CoreUtils.framework/CoreUtils",
                             RTLD_LOCAL | RTLD_LAZY);
    if (!framework) return 3;
    using Encoder = CFDataRef (*)(CFTypeRef, int32_t, int32_t*);
    using Decoder = CFTypeRef (*)(const void*, size_t, int32_t, int32_t*);
    auto encode = reinterpret_cast<Encoder>(dlsym(framework, "OPACKEncoderCreateData"));
    auto decode = reinterpret_cast<Decoder>(dlsym(framework, "OPACKDecodeBytes"));
    if (!encode || !decode) return 3;
    NSData* input = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:argv[2]]];
    if (!input || input.length > 1048576) return 4;
    NSError* error = nil;
    NSData* output = nil;
    int32_t status = 0;
    if (!strcmp(argv[1], "encode")) {
      id object = [NSPropertyListSerialization propertyListWithData:input options:0
                                                           format:nullptr error:&error];
      if (!object) return 5;
      output = (__bridge NSData*)encode((__bridge CFTypeRef)object, 0, &status);
    } else {
      CFTypeRef object = decode(input.bytes, input.length, 0, &status);
      if (!object || status) return 6;
      output = [NSPropertyListSerialization dataWithPropertyList:(__bridge id)object
                          format:NSPropertyListBinaryFormat_v1_0 options:0 error:&error];
    }
    if (status || !output) { fprintf(stderr, "platform status=%d\n", status); return 7; }
    if (![output writeToFile:[NSString stringWithUTF8String:argv[3]]
                     options:NSDataWritingWithoutOverwriting error:&error]) return 8;
    return 0;
  }
}
