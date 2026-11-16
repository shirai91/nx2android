// Android port: the subset of Mach the host uses, implemented on Linux in dm/android/compat/darwin_compat.c.
// Task / thread ports are Linux thread IDs; memory calls go through mmap / process_vm_readv; thread suspension (thread dumps) through a signal
// that parks the target in its handler. Calls with no Linux counterpart return KERN_FAILURE, and the host's callers already treat that as "not available".
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>

typedef int kern_return_t;
typedef int mach_port_t;              // a Linux thread ID (0 = none); the task port is getpid()
typedef mach_port_t thread_act_t, task_t, thread_t;
typedef mach_port_t *thread_act_array_t;
typedef unsigned int mach_msg_type_number_t;
typedef uintptr_t vm_address_t, vm_offset_t, vm_size_t;
typedef uint64_t mach_vm_address_t, mach_vm_size_t;
typedef int vm_prot_t, vm_inherit_t, boolean_t;
typedef int *thread_info_t, *task_info_t, *thread_state_t, *vm_region_info_t;
typedef int thread_flavor_t, task_flavor_t, vm_region_flavor_t;
typedef uint32_t natural_t; typedef int32_t integer_t;

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#define KERN_SUCCESS 0
#define KERN_INVALID_ADDRESS 1
#define KERN_PROTECTION_FAILURE 2
#define KERN_NO_SPACE 3
#define KERN_INVALID_ARGUMENT 4
#define KERN_FAILURE 5
#define KERN_NOT_SUPPORTED 46

#define VM_FLAGS_FIXED 0x0
#define VM_FLAGS_ANYWHERE 0x1
#define VM_FLAGS_OVERWRITE 0x4000
#define VM_INHERIT_NONE 2
#define VM_PROT_NONE 0
#define VM_PROT_READ 1
#define VM_PROT_WRITE 2
#define VM_PROT_EXECUTE 4

mach_port_t mach_task_self(void);
mach_port_t mach_thread_self(void);
kern_return_t mach_port_deallocate(task_t task, mach_port_t name);
mach_port_t pthread_mach_thread_np(pthread_t t);

kern_return_t vm_read_overwrite(task_t task, vm_address_t addr, vm_size_t size, vm_address_t data, vm_size_t *outsize);
kern_return_t vm_allocate(task_t task, vm_address_t *addr, vm_size_t size, int flags);
kern_return_t vm_deallocate(task_t task, vm_address_t addr, vm_size_t size);
// copy=TRUE: an eager copy at the target; copy=FALSE (sharing the source's pages) is impossible for private anonymous memory on Linux: KERN_NOT_SUPPORTED
kern_return_t vm_remap(task_t target, vm_address_t *addr, vm_size_t size, vm_address_t mask, int flags, task_t src_task, vm_address_t src, boolean_t copy,
                       vm_prot_t *cur, vm_prot_t *max, vm_inherit_t inherit);
kern_return_t mach_vm_remap(task_t target, mach_vm_address_t *addr, mach_vm_size_t size, mach_vm_address_t mask, int flags, task_t src_task, mach_vm_address_t src,
                            boolean_t copy, vm_prot_t *cur, vm_prot_t *max, vm_inherit_t inherit);

// ---- vm_region: from /proc/self/maps ----
#define VM_REGION_BASIC_INFO_64 9
typedef struct { vm_prot_t protection, max_protection; vm_inherit_t inheritance; boolean_t shared, reserved; uint64_t offset; int behavior; unsigned short user_wired_count; } vm_region_basic_info_data_64_t;
#define VM_REGION_BASIC_INFO_COUNT_64 ((mach_msg_type_number_t)(sizeof(vm_region_basic_info_data_64_t) / sizeof(int)))
kern_return_t vm_region_64(task_t task, vm_address_t *addr, vm_size_t *size, vm_region_flavor_t flavor, vm_region_info_t info, mach_msg_type_number_t *count, mach_port_t *obj);
kern_return_t mach_vm_region(task_t task, mach_vm_address_t *addr, mach_vm_size_t *size, vm_region_flavor_t flavor, vm_region_info_t info, mach_msg_type_number_t *count, mach_port_t *obj);

// ---- thread_info: CPU time from /proc/self/task/<tid>/stat ----
typedef struct { integer_t seconds, microseconds; } time_value_t;
#define THREAD_BASIC_INFO 3
#define TH_STATE_RUNNING 1
#define TH_STATE_WAITING 3
typedef struct { time_value_t user_time, system_time; integer_t cpu_usage, policy, run_state, flags, suspend_count, sleep_time; } thread_basic_info_data_t;
#define THREAD_BASIC_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_basic_info_data_t) / sizeof(int)))
#define THREAD_EXTENDED_INFO 5
typedef struct { uint64_t pth_user_time, pth_system_time; int32_t pth_cpu_usage, pth_policy, pth_run_state, pth_flags, pth_sleep_time, pth_curpri, pth_priority, pth_maxpriority; char pth_name[64]; } thread_extended_info_data_t;
#define THREAD_EXTENDED_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_extended_info_data_t) / sizeof(int)))
kern_return_t thread_info(thread_act_t t, thread_flavor_t flavor, thread_info_t out, mach_msg_type_number_t *count);

// ---- task_info: resident size from /proc/self/statm; no power accounting (KERN_FAILURE) ----
#define TASK_VM_INFO 22
#define TASK_POWER_INFO_V2 26
typedef struct { uint64_t virtual_size; integer_t region_count, page_size; uint64_t resident_size, resident_size_peak, device, device_peak, internal, internal_peak, external, external_peak,
                 reusable, reusable_peak, purgeable_volatile_pmap, purgeable_volatile_resident, purgeable_volatile_virtual, compressed, compressed_peak, compressed_lifetime, phys_footprint,
                 min_address, max_address; } task_vm_info_data_t;
#define TASK_VM_INFO_COUNT ((mach_msg_type_number_t)(sizeof(task_vm_info_data_t) / sizeof(int)))
typedef struct { uint64_t total_user, total_system, task_interrupt_wakeups, task_platform_idle_wakeups, task_timer_wakeups_bin_1, task_timer_wakeups_bin_2; } task_power_info_data_t;
typedef struct { task_power_info_data_t cpu_energy; struct { uint64_t task_gpu_utilisation, task_gpu_stat_reserved0, task_gpu_stat_reserved1, task_gpu_stat_reserved2; } gpu_energy;
                 uint64_t task_ptime, task_pset_switches, task_energy; } task_power_info_v2_data_t;
#define TASK_POWER_INFO_V2_COUNT ((mach_msg_type_number_t)(sizeof(task_power_info_v2_data_t) / sizeof(int)))
kern_return_t task_info(task_t task, task_flavor_t flavor, task_info_t out, mach_msg_type_number_t *count);
kern_return_t task_threads(task_t task, thread_act_array_t *list, mach_msg_type_number_t *count);

// ---- thread state: a suspended thread's registers (parked in a signal handler) ----
#define ARM_THREAD_STATE64 6
struct __darwin_arm_thread_state64 { uint64_t __x[29], __fp, __lr, __sp, __pc; uint32_t __cpsr, __pad; };
typedef struct __darwin_arm_thread_state64 arm_thread_state64_t;
#define _STRUCT_ARM_THREAD_STATE64 struct __darwin_arm_thread_state64
#define ARM_THREAD_STATE64_COUNT ((mach_msg_type_number_t)(sizeof(arm_thread_state64_t) / sizeof(int)))
#define __darwin_arm_thread_state64_get_pc(s) ((s).__pc)
#define __darwin_arm_thread_state64_get_lr(s) ((s).__lr)
#define __darwin_arm_thread_state64_get_fp(s) ((s).__fp)
#define __darwin_arm_thread_state64_get_sp(s) ((s).__sp)
kern_return_t thread_suspend(thread_act_t t);
kern_return_t thread_resume(thread_act_t t);
kern_return_t thread_get_state(thread_act_t t, thread_flavor_t flavor, thread_state_t out, mach_msg_type_number_t *count);
