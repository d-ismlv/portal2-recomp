#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

static void expected_abort(int signal_number) { (void)signal_number; _exit(77); }
static void expect_guest_fault(uint32_t address, int write_access) {
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        signal(SIGABRT, expected_abort);
        freopen("/dev/null", "w", stderr);
        if (write_access) W8(address, 1); else { volatile uint8_t value = R8(address); (void)value; }
        _exit(0);
    }
    int status; assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 77);
}
static void check_virtual_memory(void) {
    CPU c; rt_cpu_reset(&c);
    uint32_t memory = rt_mmap(&c, 0, 12288, 3, 0x22, -1, 0);
    assert(memory != UINT32_MAX && !(memory & 16383));
    W32(memory, 0x12345678); W32(memory + 8192, 0xabcdef01);
    assert(rt_mprotect(memory + 4096, 4096, 0) == 0);
    assert(R32(memory) == 0x12345678 && R32(memory + 8192) == 0xabcdef01);
    expect_guest_fault(memory + 4096, 0);
    expect_guest_fault(memory + 4096, 1);
    assert(rt_mprotect(memory + 4096, 4096, 3) == 0);
    W32(memory + 4096, 99);
    assert(rt_madvise(memory + 4096, 4096, 4) == 0);
    assert(R32(memory + 4096) == 0 && R32(memory) == 0x12345678 && R32(memory + 8192) == 0xabcdef01);
    assert(rt_mprotect(memory, 4096, 1) == 0);
    expect_guest_fault(memory, 1);
    assert(R32(memory) == 0x12345678);
    assert(rt_munmap(memory + 4096, 4096) == 0);
    expect_guest_fault(memory + 4096, 0);
    assert(rt_munmap(memory, 12288) == 0);
    expect_guest_fault(memory, 0);
    uint32_t reused = rt_mmap(&c, 0, 4096, 3, 0x22, -1, 0);
    assert(reused == memory && R32(reused) == 0);
    expect_guest_fault(reused + 4096, 0); // Darwin page padding is not guest memory
    assert(rt_mprotect(reused + 4096, 4096, 3) == -1 && errno == ENOMEM);
    assert(rt_munmap(reused, 4096) == 0);
    assert(rt_munmap(0xfffff000u, 4096) == -1);
    assert(rt_mmap(&c, 0, UINT32_MAX, 3, 0x22, -1, 0) == UINT32_MAX);
}
