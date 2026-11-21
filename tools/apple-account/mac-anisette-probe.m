/* Inspect local AOSKit support; --generate checks header availability in memory.
 * Never print/persist header values, account identities, passwords or OTPs. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <string.h>

int main(int argc, const char **argv) { @autoreleasepool {
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--generate"))) return 2;
    BOOL generate = argc == 2;
    NSMutableDictionary *report = [@{@"generation_requested": @(generate)} mutableCopy];
    void *framework = dlopen("/System/Library/PrivateFrameworks/AOSKit.framework/AOSKit", RTLD_NOW);
    report[@"framework_loaded"] = @(framework != NULL);
    Class cls = objc_getClass("AOSUtilities");
    SEL selector = sel_registerName("retrieveOTPHeadersForDSID:");
    Method method = cls ? class_getClassMethod(cls, selector) : NULL;
    report[@"method_present"] = @(method != NULL);
    BOOL compatible = NO;
    if (method) {
        NSMethodSignature *signature = [NSMethodSignature signatureWithObjCTypes:method_getTypeEncoding(method)];
        compatible = signature.numberOfArguments == 3 &&
            !strcmp(signature.methodReturnType, @encode(id)) &&
            !strcmp([signature getArgumentTypeAtIndex:2], @encode(id));
    }
    report[@"compatible_signature"] = @(compatible);
    if (generate && compatible) {
        @try {
            id (*retrieve)(id, SEL, id) = (void *)method_getImplementation(method);
            id headers = retrieve(cls, selector, @"-2");
            BOOL dictionary = [headers isKindOfClass:NSDictionary.class];
            report[@"returned_dictionary"] = @(dictionary);
            if (dictionary) report[@"returned_entry_count"] = @([headers count]);
            for (NSArray *aliases in @[@[@"X-Apple-MD", @"X-Apple-I-MD"],
                                        @[@"X-Apple-MD-M", @"X-Apple-I-MD-M"]]) {
                BOOL present = NO;
                if (dictionary) for (NSString *key in aliases) {
                    id value = headers[key];
                    if ([value isKindOfClass:NSString.class] && [value length]) present = YES;
                }
                report[[aliases[0] stringByAppendingString:@"_nonempty"]] = @(present);
            }
        } @catch (NSException *exception) {
            report[@"exception_class"] = NSStringFromClass(exception.class);
        }
    }
    NSData *json = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:NULL];
    fwrite(json.bytes, 1, json.length, stdout); putchar('\n');
    return 0;
}}
