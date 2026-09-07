#ifndef HK_TEST_MACH_H
#define HK_TEST_MACH_H
#include <stddef.h>
#include <stdint.h>
typedef uintptr_t vm_address_t;
typedef size_t vm_size_t;
typedef int mach_port_t;
typedef int kern_return_t;
typedef int vm_prot_t;
typedef unsigned mach_msg_type_number_t;
typedef struct { vm_prot_t protection; } vm_region_basic_info_data_64_t;
typedef vm_region_basic_info_data_64_t *vm_region_info_t;
#define FALSE 0
#define KERN_SUCCESS 0
#define KERN_INVALID_ADDRESS 1
#define MACH_PORT_NULL 0
#define VM_PROT_NONE 0
#define VM_PROT_READ 1
#define VM_PROT_WRITE 2
#define VM_PROT_EXECUTE 4
#define VM_PROT_COPY 16
#define VM_REGION_BASIC_INFO_64 0
#define VM_REGION_BASIC_INFO_COUNT_64 1
#define VM_FLAGS_ANYWHERE 1
#define VM_FLAGS_FIXED 0
#define VM_FLAGS_OVERWRITE 2
#define VM_INHERIT_NONE 0
#define VM_MEMORY_APPLICATION_SPECIFIC_1 0
#define VM_MAKE_TAG(x) 0
#define mach_task_self() 1
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
#endif
