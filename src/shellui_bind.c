/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "shellui_bind.h"
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <machine/reg.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <ps5/kernel.h>
#include <ps5/nid.h>

/* ptrace needs a privileged auth id; elevate for the call only. */
static int sys_ptrace(int request, pid_t pid, caddr_t addr, int data) {
    uint8_t all[16], caps[16];
    memset(all, 0xff, sizeof(all));
    pid_t me = getpid();
    uint64_t authid = kernel_get_ucred_authid(me);
    if (!authid || kernel_get_ucred_caps(me, caps)) return -1;
    if (kernel_set_ucred_authid(me, 0x4800000000010003l) || kernel_set_ucred_caps(me, all)) return -1;
    int r = (int)__syscall(SYS_ptrace, request, pid, addr, data);
    kernel_set_ucred_authid(me, authid);
    kernel_set_ucred_caps(me, caps);
    return r;
}

/* First process whose thread name is `name` (sysctl kern.proc: ki_pid@72, ki_tdname@447). */
static pid_t find_pid(const char *name) {
    int mib[4] = {1, 14, 8, 0};
    size_t size = 0;
    pid_t found = -1;
    if (sysctl(mib, 4, NULL, &size, NULL, 0)) return -1;
    uint8_t *buf = malloc(size);
    if (!buf) return -1;
    if (sysctl(mib, 4, buf, &size, NULL, 0) == 0) {
        for (uint8_t *p = buf; p < buf + size; p += *(int *)p) {
            if (*(int *)p <= 0) break;
            pid_t pid = *(pid_t *)&p[72];
            if (pid != getpid() && !strcmp((char *)&p[447], name) && (found < 0 || pid < found)) found = pid;
        }
    }
    free(buf);
    return found;
}

static uint32_t lib_handle(pid_t pid, const char *name) {
    uint32_t h = 0;
    if (kernel_dynlib_handle(pid, name, &h) != 0 || !h) {
        char sprx[64]; strcpy(sprx, name); strcat(sprx, ".sprx");
        h = 0; kernel_dynlib_handle(pid, sprx, &h);
    }
    return h;
}

static intptr_t resolve(pid_t pid, uint32_t lib, const char *sym) {
    intptr_t a = kernel_dynlib_dlsym(pid, lib, sym);
    if (!a) { char nid[12]; nid_encode(sym, nid); a = kernel_dynlib_resolve(pid, lib, nid); }
    return a;
}

static int pt_io(pid_t pid, int op, intptr_t remote, void *local, size_t len) {
    struct ptrace_io_desc io = { .piod_op = op, .piod_offs = (void *)remote,
                                 .piod_addr = local, .piod_len = len };
    return sys_ptrace(PT_IO, pid, (caddr_t)&io, 0);
}

/* Call fn(a1, a2) in the stopped process; it returns into an INT3 at trap. */
static int64_t pt_call(pid_t pid, intptr_t fn, intptr_t trap, uint64_t a1, uint64_t a2,
                       int *returned) {
    struct reg regs, saved;
    int status;
    *returned = 0;
    if (sys_ptrace(PT_GETREGS, pid, (caddr_t)&regs, 0)) return -1;
    saved = regs;
    intptr_t rsp = (regs.r_rsp - 256) & ~(intptr_t)0xf;   /* past the red zone */
    if (pt_io(pid, PIOD_WRITE_D, rsp, &trap, 8)) return -1;
    regs.r_rsp = rsp; regs.r_rip = fn; regs.r_rdi = a1; regs.r_rsi = a2;
    if (sys_ptrace(PT_SETREGS, pid, (caddr_t)&regs, 0) ||
        sys_ptrace(PT_CONTINUE, pid, (caddr_t)1, 0)) {
        sys_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
        return -1;
    }
    int trapped = 0;
    for (int ms = 0; ms < 5000 && !trapped; ) {
        int r = waitpid(pid, &status, WNOHANG);
        if (r < 0) break;
        if (r == 0) { usleep(1000); ms++; continue; }
        if (!WIFSTOPPED(status)) break;
        int sig = WSTOPSIG(status);
        if (sig == SIGTRAP) { trapped = 1; break; }
        /* Forwarding SIGCHLD into SceShellUI during injected code panicked
         * the kernel (upstream); swallow it, pass anything else on. */
        sys_ptrace(PT_CONTINUE, pid, (caddr_t)1, sig == SIGCHLD ? 0 : sig);
    }
    int64_t ret = -1;
    *returned = trapped;
    if (trapped && sys_ptrace(PT_GETREGS, pid, (caddr_t)&regs, 0) == 0) ret = (int64_t)regs.r_rax;
    sys_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
    return ret;
}

int shellui_bind(uint64_t device_id, int32_t user_id) {
    pid_t pid = find_pid("SceShellUI");
    if (pid < 0) return -1;
    if (sys_ptrace(PT_ATTACH, pid, 0, 0) != 0) return -1;
    waitpid(pid, NULL, 0);

    int64_t ret = -1;
    uint32_t mbus = lib_handle(pid, "libSceMbus");
    uint32_t pad  = lib_handle(pid, "libScePad");
    intptr_t fn   = mbus ? resolve(pid, mbus, "sceMbusBindDeviceWithUserId") : 0;
    /* Trap site: libScePad's init (or fini) routine, which is not running.
     * Its first byte and page protection are put back after the call. */
    intptr_t trap = pad ? kernel_dynlib_init_addr(pid, pad) : 0;
    if (!trap && pad) trap = kernel_dynlib_fini_addr(pid, pad);
    if (fn && trap) {
        uint8_t orig = 0, int3 = 0xCC;
        int prot = kernel_get_vmem_protection(pid, trap, 1);
        kernel_set_vmem_protection(pid, trap, 1, PROT_READ | PROT_WRITE | PROT_EXEC);
        int have_orig = pt_io(pid, PIOD_READ_D, trap, &orig, 1) == 0;
        if (pt_io(pid, PIOD_WRITE_D, trap, &int3, 1) == 0) {
            int returned;
            ret = pt_call(pid, fn, trap, device_id, (uint32_t)user_id, &returned);
            /* A call that never came back still needs its trap. */
            if (have_orig && returned) pt_io(pid, PIOD_WRITE_D, trap, &orig, 1);
        }
        if (prot > 0) kernel_set_vmem_protection(pid, trap, 1, prot);
    }
    sys_ptrace(PT_DETACH, pid, (caddr_t)1, 0);
    return ret == 0 ? 0 : -1;
}
