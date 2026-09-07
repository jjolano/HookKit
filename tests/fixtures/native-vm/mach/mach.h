#ifndef HK_TEST_MACH_H
#define HK_TEST_MACH_H
#include <stddef.h>
#include <stdint.h>
#if !defined(mach_port_t) && !defined(__darwin_mach_port_t) && !defined(_MACH_PORT_T)
// On macOS the real <mach/mach.h> may already be pulled in (e.g. via
// pthread.h) before hk_native.c's include resolves here. Guard every name
// so the fixture degrades to "already provided" instead of redefinition.
#ifndef vm_address_t
typedef uintptr_t vm_address_t;
#endif
#ifndef vm_size_t
typedef size_t vm_size_t;
#endif
#ifndef mach_port_t
typedef int mach_port_t;
#endif
#ifndef kern_return_t
typedef int kern_return_t;
#endif
#ifndef vm_prot_t
typedef int vm_prot_t;
#endif
#ifndef mach_msg_type_number_t
typedef unsigned mach_msg_type_number_t;
#endif
#ifndef vm_region_basic_info_data_64_t
typedef struct { vm_prot_t protection; } vm_region_basic_info_data_64_t;
#endif
#ifndef vm_region_info_t
typedef vm_region_basic_info_data_64_t *vm_region_info_t;
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef KERN_SUCCESS
#define KERN_SUCCESS 0
#endif
#ifndef KERN_INVALID_ADDRESS
#define KERN_INVALID_ADDRESS 1
#endif
#ifndef MACH_PORT_NULL
#define MACH_PORT_NULL 0
#endif
#ifndef VM_PROT_NONE
#define VM_PROT_NONE 0
#endif
#ifndef VM_PROT_READ
#define VM_PROT_READ 1
#endif
#ifndef VM_PROT_WRITE
#define VM_PROT_WRITE 2
#endif
#ifndef VM_PROT_EXECUTE
#define VM_PROT_EXECUTE 4
#endif
#ifndef VM_PROT_COPY
#define VM_PROT_COPY 16
#endif
#ifndef VM_REGION_BASIC_INFO_64
#define VM_REGION_BASIC_INFO_64 0
#endif
#ifndef VM_REGION_BASIC_INFO_COUNT_64
#define VM_REGION_BASIC_INFO_COUNT_64 1
#endif
#ifndef VM_FLAGS_ANYWHERE
#define VM_FLAGS_ANYWHERE 1
#endif
#ifndef VM_FLAGS_FIXED
#define VM_FLAGS_FIXED 0
#endif
#ifndef VM_FLAGS_OVERWRITE
#define VM_FLAGS_OVERWRITE 2
#endif
#ifndef VM_INHERIT_NONE
#define VM_INHERIT_NONE 0
#endif
#ifndef VM_MEMORY_APPLICATION_SPECIFIC_1
#define VM_MEMORY_APPLICATION_SPECIFIC_1 0
#endif
#ifndef VM_MAKE_TAG
#define VM_MAKE_TAG(x) 0
#endif
#ifndef mach_task_self
#define mach_task_self() 1
#endif
// Keep fixture functions from interposing on host system-library VM calls.
#define vm_region_64 hk_test_vm_region_64
#define mach_port_deallocate hk_test_mach_port_deallocate
#define vm_protect hk_test_vm_protect
#define vm_allocate hk_test_vm_allocate
#define vm_deallocate hk_test_vm_deallocate
#define vm_remap hk_test_vm_remap
kern_return_t vm_region_64(mach_port_t, vm_address_t *, vm_size_t *, int,
                           vm_region_info_t, mach_msg_type_number_t *, mach_port_t *);
kern_return_t mach_port_deallocate(mach_port_t, mach_port_t);
kern_return_t vm_protect(mach_port_t, vm_address_t, vm_size_t, int, vm_prot_t);
kern_return_t vm_allocate(mach_port_t, vm_address_t *, vm_size_t, int);
kern_return_t vm_deallocate(mach_port_t, vm_address_t, vm_size_t);
kern_return_t vm_remap(mach_port_t, vm_address_t *, vm_size_t, vm_address_t,
                       int, mach_port_t, vm_address_t, int, vm_prot_t *, vm_prot_t *, int);
#endif // fixture types skipped: real <mach/mach.h> already included
#endif
