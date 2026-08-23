// Host-test stub for Apple's <Foundation/Foundation.h>: lets the test include
// the real Headers/HookKit.h on Linux. HookKit.h only uses these names
// in declarations; the test never touches them at runtime.
#ifndef fake_foundation_h
#define fake_foundation_h
#include <stddef.h>
typedef unsigned long NSUInteger;
typedef signed char BOOL;
#define YES ((BOOL)1)
#define NO ((BOOL)0)
#ifndef nil
#define nil ((id)0)
#endif
#define NS_ENUM(_type, _name) enum _name : _type _name; enum _name : _type
@interface NSObject
+ (id)new;
@end
@interface NSArray <__covariant ObjectType>
+ (instancetype)arrayWithObjects:(const id [])objects count:(NSUInteger)count;
@end
@interface NSDictionary <__covariant KeyType, __covariant ObjectType> @end
@interface NSNumber @end
@interface NSValue @end
@interface NSString @end
#endif
