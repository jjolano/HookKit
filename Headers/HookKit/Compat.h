// HookKit v1 compatibility path. v1 sources reached HKSubstitutor, the status/
// lib enums and the HK* convenience macros through <HookKit/Compat.h>; the 3.0
// facade umbrella at <HookKit.h> is a strict superset of that contract (every
// v1 enumerator keeps its v1 value), so forward rather than declare twice.
#ifndef hookkit_compat_h
#define hookkit_compat_h

#import <HookKit.h>

#endif
