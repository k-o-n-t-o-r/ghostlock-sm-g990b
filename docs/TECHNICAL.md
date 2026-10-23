# Technical notes

This port targets one kernel build. It is not a generic Samsung exploit.

## Chain

1. **KASLR**: tracefs exposes a known worker caller. Its runtime address gives the image slide.
2. **Heap address**: KernelSnitch leaks `mm_struct` addresses through futex-hash collision timing. The collector holds all 34 objects in one order-3 slab.
3. **FOPS payload**: the slab is discarded and reclaimed with payload-backed io_uring blocks at orders 3 through 9, followed by an order-3 skb spray.
4. **Stack overlay**: `pselect6` copies its result bitmap over the stale waiter. A COW fault blocks copy-out under `mmap_sem`, leaving enough time to trigger `sched_setattr` while the controlled words are live.
5. **Arbitrary write**: the PI walk reaches `rb_erase`. Its one-right-child case redirects `ashmem_misc.fops` to a fake table without corrupting `owner`.
6. **Kernel R/W**: the fake table routes ashmem reads and writes through configfs callbacks. A second controlled slab is watched through this read primitive until a marked order-3 skb owns the exact page. Freeing only that skb lets kmalloc-1k pipe-buffer arrays take it. Forged pipe buffers provide page-granular physical read/write.
7. **Root**: the exploit clears `selinux_enforcing`, inserts a synthetic work item into `system_unbound_wq`, and calls `call_usermodehelper_exec_work.cfi_jt`. The helper starts a uid-0 daemon and the normal client verifies `id -u == 0`.

![GhostLock SM-G990B exploit chain](exploit-chain.svg)

The editable diagram source is [`exploit-chain.html`](exploit-chain.html).

## Device-specific details

| Item | Value |
| --- | --- |
| Device | Samsung Galaxy S21 FE 5G (`SM-G990B`, `r9q`) |
| Firmware | `G990BXXSLIZH2` |
| Android | 16 / One UI 8 |
| Kernel | `5.4.289-qgki-32192773-abG990BXXSLIZH2` |
| SoC | Snapdragon 888 |
| `mm_struct` SLUB stride | `0x3c0`, 34 objects in an order-3 slab |
| Pipe array | 16 slots, 640 bytes, kmalloc-1k, 32 arrays per order-3 slab |

The target header contains all recovered image symbols, CFI jump-table addresses, and randomized-structure offsets. Kallsyms were recovered from the exact LIZH2 boot image rather than copied from a nearby firmware.

## Reliability rules

- Force reboot before every attempt and wait for `sys.boot_completed=1`.
- Run one controlled attempt per boot.
- Never retry after the PI writer has run, even if userspace returned normally.
- The exploit deliberately sets SELinux permissive. Root and permissive mode disappear after reboot.
- The detached keeper retains reclaimed pages while the root daemon is alive.

Two consecutive independent clean-boot runs completed the full chain, ran the root client, restored modified ashmem and slide globals, and exited with status 0.
