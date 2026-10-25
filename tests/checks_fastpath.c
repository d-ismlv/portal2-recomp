// Chemin rapide du code généré : invariants dont dépendent RT_FAST_MEMORY et
// les appels directs (tools/lift.py).
#include "checks.h"
#include <mach/mach.h>
#include <mach/mach_vm.h>

// Sans contrôle logiciel, un accès de 8 ou 16 octets commençant juste sous
// 4 Go déborde de l'espace invité : la page qui suit doit appartenir à la
// réservation du runtime, sans aucun droit.
RT_CHECK(fastpath_guard_after_4g) {
    mach_vm_address_t probe = (mach_vm_address_t)(uintptr_t)(M + (UINT64_C(1) << 32));
    mach_vm_address_t region = probe;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    assert(mach_vm_region(mach_task_self(), &region, &size, VM_REGION_BASIC_INFO_64,
                          (vm_region_info_t)&info, &count, &object) == KERN_SUCCESS);
    assert(region <= probe && probe + 16 <= region + size);
    assert(info.protection == VM_PROT_NONE);
}
