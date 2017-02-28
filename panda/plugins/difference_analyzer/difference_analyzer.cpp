/* PANDABEGINCOMMENT
 * 
 * Authors:
 *  Tim Leek               tleek@ll.mit.edu
 *  Ryan Whelan            rwhelan@ll.mit.edu
 *  Joshua Hodosh          josh.hodosh@ll.mit.edu
 *  Michael Zhivich        mzhivich@ll.mit.edu
 *  Brendan Dolan-Gavitt   brendandg@gatech.edu
 * 
 * This work is licensed under the terms of the GNU GPL, version 2. 
 * See the COPYING file in the top-level directory. 
 * 
PANDAENDCOMMENT */
// This needs to be defined before anything is included in order to get
// the PRIx64 macro
#define __STDC_FORMAT_MACROS

#include <cstdio>
#include <map>
#include <iostream>
#include "panda/plugin.h"
#include "panda/plugin_plugin.h"
#include "panda/plugin.h"

// These need to be extern "C" so that the ABI is compatible with
// QEMU/PANDA, which is written in C
extern "C" {
#include "panda/rr/rr_log.h"
#include "panda/addr.h"
#include "panda/plog.h"

#include "osi/osi_types.h"
#include "osi/osi_ext.h"

// this provides the fd resolution magic
#include "osi_linux/osi_linux_ext.h"

#include "syscalls2/gen_syscalls_ext_typedefs.h"

bool init_plugin(void *);
void uninit_plugin(void *);

}

bool debug = true;
//std::ostream outfile;

#define MAX_FILENAME 256
std::map <target_ulong, OsiProc> running_procs;

void die(const char* fmt, ...) {
    va_list args;

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

uint32_t guest_strncpy(CPUState *cpu, char *buf, size_t maxlen, target_ulong guest_addr) {
    buf[0] = 0;
    unsigned i;
    for (i=0; i<maxlen; i++) {
        uint8_t c;
        panda_virtual_memory_rw(cpu, guest_addr+i, &c, 1, 0);
        buf[i] = c;
        if (c==0) {
            break;
        }
    }
    buf[maxlen-1] = 0;
    return i;
}

#if defined(TARGET_I386)

void linux_mmap_pgoff_return(CPUState *cpu,target_ulong pc,uint32_t addr,uint32_t len,uint32_t prot,uint32_t flags,uint32_t fd,uint32_t pgoff) {
    CPUArchState *env = (CPUArchState*)cpu->env_ptr;
    target_ulong asid = panda_current_asid(cpu);
    if (running_procs.count(asid) == 0) {
        //printf ("linux_mmap_pgoff_enter for asid=0x%x fd=%d -- dont know about that asid.  discarding \n", (unsigned int) asid, (int) fd);
        return;
    }
    if ((int32_t) fd == -1){
        //printf ("linux_mmap_pgoff_enter for asid=0x%x fd=%d flags=%x -- not valid fd . . . \n", (unsigned int) asid, (int) fd, flags);
        return;
    }
    OsiProc proc = running_procs[asid];
    char *filename = osi_linux_fd_to_filename(cpu, &proc, fd);
    // gets us offset into the file.  could be useful
    //uint64_t pos = osi_linux_fd_to_pos(env, &proc, fd);
    // if a filename exists and permission is executable
    // TODO: fix this magic constant of 0x04 for PROT_EXEC
    if (filename != NULL && ((prot & 0x04) == 0x04)) {
        if (debug) {
            printf ("[difference_analyzer] linux_mmap_pgoff(fd=%d filename=[%s] "
                    "len=%d prot=%x flags=%x "
                    "pgoff=%d)=" TARGET_FMT_lx "\n", (int) fd,
                    filename, len, prot, flags, pgoff, env->regs[R_EAX]);
        }
    } else if ((prot & 0x04) == 0x04) {
        printf ("[difference_analyzer] linux_mmap_pgoff(fd=%d "
                "len=%d prot=%x flags=%x "
                "pgoff=%d)=" TARGET_FMT_lx "\n", (int) fd,
                len, prot, flags, pgoff, env->regs[R_EAX]);
    }
}
#endif

// get current process before each bb execs
// which will probably help us actually know the current process
int osi_foo(CPUState *cpu, TranslationBlock *tb) {

    if (panda_in_kernel(cpu)) {

        OsiProc *p = get_current_process(cpu);

        //some sanity checks on what we think the current process is
        // this means we didnt find current task
        if (p->offset == 0) return 0;
        // or the name
        if (p->name == 0) return 0;
        // this is just not ok
        if (((int) p->pid) == -1) return 0;
        uint32_t n = strnlen(p->name, 32);
        // name is one char?
        if (n<2) return 0;
        uint32_t np = 0;
        for (uint32_t i=0; i<n; i++) {
            np += (isprint(p->name[i]) != 0);
        }
        // name doesnt consist of solely printable characters
        //        printf ("np=%d n=%d\n", np, n);
        if (np != n) return 0;
        target_ulong asid = panda_current_asid(cpu);
        if (running_procs.count(asid) == 0) {
            printf ("adding asid=0x%x to running procs.  cmd=[%s]  task=0x%x\n", (unsigned int)  asid, p->name, (unsigned int) p->offset);
        }
        running_procs[asid] = *p;
    }

    return 0;
}

void handle_sys(CPUState *cpu, target_ulong pc, target_ulong callno, bool isEnter) {
   target_ulong asid = panda_current_asid(cpu);
   auto it = running_procs.find(asid);
   assert(it != running_procs.end());
   std::cout << std::hex << "0x" << pc << ", " <<
   asid << ", " <<
   std::dec <<
   (it->second).name << ", " <<
   callno << ", " <<
   isEnter << "\n";
}
void sys_enter(CPUState *cpu, target_ulong pc, target_ulong callno) {
   handle_sys(cpu, pc, callno, true);

}
void sys_return(CPUState *cpu, target_ulong pc, target_ulong callno) {
   handle_sys(cpu, pc, callno, false);

}
void sys_enter_unknown(CPUState *cpu, target_ulong pc, target_ulong callno) {
   handle_sys(cpu, pc, callno, true);

}
void sys_return_unknown(CPUState *cpu, target_ulong pc, target_ulong callno) {
   handle_sys(cpu, pc, callno, false);

}
bool init_plugin(void *self) {
    //panda_arg_list *args = panda_get_args("difference_analyzer");
    //outfile = open("out");
    std::cout << "***IN DIFFERENCE ANALYZER!****\n";

    panda_require("osi");
    assert(init_osi_api());
    panda_require("osi_linux");
    assert(init_osi_linux_api());
    panda_require("syscalls2");

#if defined(TARGET_I386)
    {
        panda_cb pcb;
        pcb.before_block_exec = osi_foo;
        panda_register_callback(self, PANDA_CB_BEFORE_BLOCK_EXEC, pcb);
    }

    //PPP_REG_CB("syscalls2", on_sys_mmap_pgoff_return, linux_mmap_pgoff_return);
    PPP_REG_CB("syscalls2", on_all_sys_enter, sys_enter);
    PPP_REG_CB("syscalls2", on_all_sys_return, sys_return);
    PPP_REG_CB("syscalls2", on_unknown_sys_enter, sys_enter_unknown);
    PPP_REG_CB("syscalls2", on_unknown_sys_return, sys_return_unknown);
    // don't use these at them moment
    //PPP_REG_CB("syscalls2", on_sys_old_mmap_return, linux_old_mmap_return);
    //PPP_REG_CB("syscalls2", on_sys_mprotect_return, linux_mprotect_return);
#else
    fprintf(stderr, "The difference_analyzer plugin is not currently supported on this platform.\n");
    return false;
#endif
    return true;
}

void uninit_plugin(void *self) { }
