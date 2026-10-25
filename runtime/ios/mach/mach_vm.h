// iOS : <mach/mach_vm.h> est refusé par le SDK. rt.c ne s'en sert que pour
// mach_vm_region (diagnostic de lisibilité) : équivalent vm_region_64, de même
// signature sur arm64 (adresses et tailles 64 bits).
#pragma once
#include <mach/mach.h>
#define mach_vm_region(task, address, size, flavor, info, count, object) \
    vm_region_64((task), (vm_address_t *)(address), (vm_size_t *)(size), (flavor), (info), (count), (object))
