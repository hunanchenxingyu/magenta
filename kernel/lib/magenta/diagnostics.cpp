// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <magenta/diagnostics.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <kernel/auto_lock.h>
#include <lib/console.h>

#include <magenta/job_dispatcher.h>
#include <magenta/magenta.h>
#include <magenta/process_dispatcher.h>

// Machinery to walk over a job tree and run a callback on each process.
template <typename ProcessCallbackType>
class ProcessWalker final : public JobEnumerator {
public:
    ProcessWalker(ProcessCallbackType cb) : cb_(cb) {}
    ProcessWalker(const ProcessWalker&) = delete;
    ProcessWalker(ProcessWalker&& other) : cb_(other.cb_) {}

private:
    bool Size(uint32_t proc_count, uint32_t job_count) final {
        return true;
    }

    bool OnJob(JobDispatcher* job, uint32_t index) final {
        return true;
    }

    bool OnProcess(ProcessDispatcher* process, uint32_t index) final {
        cb_(process);
        return true;
    }

    ProcessCallbackType cb_;
};

template <typename ProcessCallbackType>
static ProcessWalker<ProcessCallbackType> MakeProcessWalker(ProcessCallbackType cb) {
    return ProcessWalker<ProcessCallbackType>(cb);
}

static void DumpProcessListKeyMap() {
    printf("id  : process id number\n");
    printf("-s  : state: R = running D = dead\n");
    printf("#t  : number of threads\n");
    printf("#pg : number of allocated pages\n");
    printf("#h  : total number of handles\n");
    printf("#jb : number of job handles\n");
    printf("#pr : number of process handles\n");
    printf("#th : number of thread handles\n");
    printf("#vo : number of vmo handles\n");
    printf("#vm : number of virtual memory address region handles\n");
    printf("#ch : number of channel handles\n");
    printf("#ev : number of event and event pair handles\n");
    printf("#ip : number of io port handles\n");
}

static char StateChar(const ProcessDispatcher& pd) {
    switch (pd.state()) {
        case ProcessDispatcher::State::INITIAL:
            return 'I';
        case ProcessDispatcher::State::RUNNING:
            return 'R';
        case ProcessDispatcher::State::DYING:
            return 'Y';
        case ProcessDispatcher::State::DEAD:
            return 'D';
    }
    return '?';
}

static const char* ObjectTypeToString(mx_obj_type_t type) {
    static_assert(MX_OBJ_TYPE_LAST == 23, "need to update switch below");

    switch (type) {
        case MX_OBJ_TYPE_PROCESS: return "process";
        case MX_OBJ_TYPE_THREAD: return "thread";
        case MX_OBJ_TYPE_VMEM: return "vmo";
        case MX_OBJ_TYPE_CHANNEL: return "channel";
        case MX_OBJ_TYPE_EVENT: return "event";
        case MX_OBJ_TYPE_IOPORT: return "io-port";
        case MX_OBJ_TYPE_INTERRUPT: return "interrupt";
        case MX_OBJ_TYPE_IOMAP: return "io-map";
        case MX_OBJ_TYPE_PCI_DEVICE: return "pci-device";
        case MX_OBJ_TYPE_LOG: return "log";
        case MX_OBJ_TYPE_WAIT_SET: return "wait-set";
        case MX_OBJ_TYPE_SOCKET: return "socket";
        case MX_OBJ_TYPE_RESOURCE: return "resource";
        case MX_OBJ_TYPE_EVENT_PAIR: return "event-pair";
        case MX_OBJ_TYPE_JOB: return "job";
        case MX_OBJ_TYPE_VMAR: return "vmar";
        case MX_OBJ_TYPE_FIFO: return "fifo";
        case MX_OBJ_TYPE_IOPORT2: return "portv2";
        case MX_OBJ_TYPE_HYPERVISOR: return "hypervisor";
        case MX_OBJ_TYPE_GUEST: return "guest";
        default: return "???";
    }
}

uint32_t BuildHandleStats(const ProcessDispatcher& pd, uint32_t* handle_type, size_t size) {
    AutoLock lock(&pd.handle_table_lock_);
    uint32_t total = 0;
    for (const auto& handle : pd.handles_) {
        if (handle_type) {
            uint32_t type = static_cast<uint32_t>(handle.dispatcher()->get_type());
            if (size > type)
                ++handle_type[type];
        }
        ++total;
    }
    return total;
}

uint32_t ProcessDispatcher::ThreadCount() const {
    AutoLock lock(&state_lock_);
    return static_cast<uint32_t>(thread_list_.size_slow());
}

size_t ProcessDispatcher::PageCount() const {
    return aspace_->AllocatedPages();
}

static char* DumpHandleTypeCountLocked(const ProcessDispatcher& pd) {
    static char buf[(MX_OBJ_TYPE_LAST * 4) + 1];

    uint32_t types[MX_OBJ_TYPE_LAST] = {0};
    uint32_t handle_count = BuildHandleStats(pd, types, sizeof(types));

    snprintf(buf, sizeof(buf), "%3u: %3u %3u %3u %3u %3u %3u %3u %3u",
             handle_count,
             types[MX_OBJ_TYPE_JOB],
             types[MX_OBJ_TYPE_PROCESS],
             types[MX_OBJ_TYPE_THREAD],
             types[MX_OBJ_TYPE_VMEM],
             types[MX_OBJ_TYPE_VMAR],
             types[MX_OBJ_TYPE_CHANNEL],
             // Events and event pairs:
             types[MX_OBJ_TYPE_EVENT] + types[MX_OBJ_TYPE_EVENT_PAIR],
             types[MX_OBJ_TYPE_IOPORT]
             );
    return buf;
}

void DumpProcessList() {
    printf("%8s-s  #t  #pg  #h:  #jb #pr #th #vo #vm #ch #ev #ip [  job:name]\n", "id");

    auto walker = MakeProcessWalker([](ProcessDispatcher* process) {
        char pname[MX_MAX_NAME_LEN];
        process->get_name(pname);
        printf("%8" PRIu64 "-%c %3u %4zu %s  [%5" PRIu64 ":%s]\n",
               process->get_koid(),
               StateChar(*process),
               process->ThreadCount(),
               process->PageCount(),
               DumpHandleTypeCountLocked(*process),
               process->get_related_koid(),
               pname);
    });
    GetRootJobDispatcher()->EnumerateChildren(&walker);
}

void DumpProcessHandles(mx_koid_t id) {
    auto pd = ProcessDispatcher::LookupProcessById(id);
    if (!pd) {
        printf("process not found!\n");
        return;
    }

    printf("process [%" PRIu64 "] handles :\n", id);
    printf("handle       koid : type\n");

    AutoLock lock(&pd->handle_table_lock_);
    uint32_t total = 0;
    for (const auto& handle : pd->handles_) {
        auto type = handle.dispatcher()->get_type();
        printf("%9d %7" PRIu64 " : %s\n",
            pd->MapHandleToValue(&handle),
            handle.dispatcher()->get_koid(),
            ObjectTypeToString(type));
        ++total;
    }
    printf("total: %u handles\n", total);
}


class JobDumper final : public JobEnumerator {
public:
    JobDumper(mx_koid_t self) : self_(self) {}
    JobDumper(const JobDumper&) = delete;

private:
    bool Size(uint32_t proc_count, uint32_t job_count) final {
        if (!job_count)
            printf("no jobs\n");
        if (proc_count < 2)
            printf("no processes\n");
        return true;
    }

    // This is called by JobDispatcher::EnumerateChildren() which acquires JobDispatcher::lock_
    // first, making it safe to access job->process_count_ etc, but there's no reasonable way to
    // express this fact via thread safety annotations so we disable the analysis for this function.
    bool OnJob(JobDispatcher* job, uint32_t index) final TA_NO_THREAD_SAFETY_ANALYSIS {
        printf("- %" PRIu64 " child job (%" PRIu32 " processes)\n",
            job->get_koid(), job->process_count());
        return true;
    }

    bool OnProcess(ProcessDispatcher* proc, uint32_t index) final {
        auto id = proc->get_koid();
        if (id != self_) {
            char pname[MX_MAX_NAME_LEN];
            proc->get_name(pname);
            printf("- %" PRIu64 " proc [%s]\n", id, pname);
        }

        return true;
    }

    mx_koid_t self_;
};

void DumpJobTreeForProcess(mx_koid_t id) {
    auto pd = ProcessDispatcher::LookupProcessById(id);
    if (!pd) {
        printf("process not found!\n");
        return;
    }

    auto job = pd->job();
    if (!job) {
        printf("process has no job!!\n");
        return;
    }

    char pname[MX_MAX_NAME_LEN];
    pd->get_name(pname);

    printf("process %" PRIu64 " [%s]\n", id, pname);
    printf("in job [%" PRIu64 "]", job->get_koid());

    auto parent = job;
    while (true) {
        parent = parent->parent();
        if (!parent)
            break;
        printf("-->[%" PRIu64 "]", parent->get_koid());
    }
    printf("\n");

    JobDumper dumper(id);
    job->EnumerateChildren(&dumper);
}

void KillProcess(mx_koid_t id) {
    // search the process list and send a kill if found
    auto pd = ProcessDispatcher::LookupProcessById(id);
    if (!pd) {
        printf("process not found!\n");
        return;
    }
    // if found, outside of the lock hit it with kill
    printf("killing process %" PRIu64 "\n", id);
    pd->Kill();
}

namespace {
// Counts memory usage under a VmAspace.
class VmCounter final : public VmEnumerator {
public:
    bool OnVmMapping(const VmMapping* map, const VmAddressRegion* vmar,
                     uint depth) override {
        usage.mapped_pages += map->size() / PAGE_SIZE;
        usage.committed_pages += map->vmo()->AllocatedPagesInRange(
            map->object_offset(), map->size());
        return true;
    }

    VmAspace::vm_usage_t usage = {};
};
} // namespace

status_t VmAspace::GetMemoryUsage(vm_usage_t* usage) {
    VmCounter vc;
    if (!EnumerateChildren(&vc)) {
        *usage = {};
        return ERR_INTERNAL;
    }
    *usage = vc.usage;
    return NO_ERROR;
}

namespace {
unsigned int arch_mmu_flags_to_vm_flags(unsigned int arch_mmu_flags) {
    if (arch_mmu_flags & ARCH_MMU_FLAG_INVALID) {
        return 0;
    }
    unsigned int ret = 0;
    if (arch_mmu_flags & ARCH_MMU_FLAG_PERM_READ) {
        ret |= MX_VM_FLAG_PERM_READ;
    }
    if (arch_mmu_flags & ARCH_MMU_FLAG_PERM_WRITE) {
        ret |= MX_VM_FLAG_PERM_WRITE;
    }
    if (arch_mmu_flags & ARCH_MMU_FLAG_PERM_EXECUTE) {
        ret |= MX_VM_FLAG_PERM_EXECUTE;
    }
    return ret;
}

// Builds a description of an apsace/vmar/mapping hierarchy.
class VmMapBuilder final : public VmEnumerator {
public:
    // NOTE: Code outside of the syscall layer should not typically know about
    // user_ptrs; do not use this pattern as an example.
    VmMapBuilder(user_ptr<mx_info_maps_t> maps, size_t max)
        : maps_(maps), max_(max) {}

    bool OnVmAddressRegion(const VmAddressRegion* vmar, uint depth) override {
        available_++;
        if (nelem_ < max_) {
            mx_info_maps_t entry = {};
            strncpy(entry.name, vmar->name(), sizeof(entry.name));
            entry.base = vmar->base();
            entry.size = vmar->size();
            entry.depth = depth + 1; // The root aspace is depth 0.
            entry.type = MX_INFO_MAPS_TYPE_VMAR;
            if (maps_.copy_array_to_user(&entry, 1, nelem_) != NO_ERROR) {
                return false;
            }
            nelem_++;
        }
        return true;
    }

    bool OnVmMapping(const VmMapping* map, const VmAddressRegion* vmar,
                     uint depth) override {
        available_++;
        if (nelem_ < max_) {
            mx_info_maps_t entry = {};
            strncpy(entry.name, map->name(), sizeof(entry.name));
            entry.base = map->base();
            entry.size = map->size();
            entry.depth = depth + 1; // The root aspace is depth 0.
            entry.type = MX_INFO_MAPS_TYPE_MAPPING;
            mx_info_maps_mapping_t* u = &entry.u.mapping;
            u->mmu_flags =
                arch_mmu_flags_to_vm_flags(map->arch_mmu_flags());
            u->committed_pages = map->vmo()->AllocatedPagesInRange(
                map->object_offset(), map->size());
            if (maps_.copy_array_to_user(&entry, 1, nelem_) != NO_ERROR) {
                return false;
            }
            nelem_++;
        }
        return true;
    }

    size_t nelem() const { return nelem_; }
    size_t available() const { return available_; }

private:
    // The caller must write an entry for the root VmAspace at index 0.
    size_t nelem_ = 1;
    size_t available_ = 1;
    user_ptr<mx_info_maps_t> maps_;
    size_t max_;
};
} // namespace

// NOTE: Code outside of the syscall layer should not typically know about
// user_ptrs; do not use this pattern as an example.
status_t GetVmAspaceMaps(mxtl::RefPtr<VmAspace> aspace,
                         user_ptr<mx_info_maps_t> maps, size_t max,
                         size_t* actual, size_t* available) {
    DEBUG_ASSERT(aspace != nullptr);
    *actual = 0;
    *available = 0;
    if (aspace->is_destroyed()) {
        return ERR_BAD_STATE;
    }
    if (max > 0) {
        mx_info_maps_t entry = {};
        strncpy(entry.name, aspace->name(), sizeof(entry.name));
        entry.base = aspace->base();
        entry.size = aspace->size();
        entry.depth = 0;
        entry.type = MX_INFO_MAPS_TYPE_ASPACE;
        if (maps.copy_array_to_user(&entry, 1, 0) != NO_ERROR) {
            return ERR_INVALID_ARGS;
        }
    }

    VmMapBuilder b(maps, max);
    if (!aspace->EnumerateChildren(&b)) {
        // VmMapBuilder only returns false
        // when it can't copy to the user pointer.
        return ERR_INVALID_ARGS;
    }
    *actual = max > 0 ? b.nelem() : 0;
    *available = b.available();
    return NO_ERROR;
}

void DumpProcessAddressSpace(mx_koid_t id) {
    auto pd = ProcessDispatcher::LookupProcessById(id);
    if (!pd) {
        printf("process not found!\n");
        return;
    }

    pd->aspace()->Dump(true);
}

static size_t mwd_limit = 32 * 256;
static bool mwd_running;

void DumpProcessMemoryUsage(const char* prefix, size_t limit) {
    auto walker = MakeProcessWalker([&](ProcessDispatcher* process) {
        size_t pages = process->PageCount();
        if (pages > limit) {
            char pname[MX_MAX_NAME_LEN];
            process->get_name(pname);
            printf("%s%s: %zu MB\n", prefix, pname, pages / 256);
        }
    });
    GetRootJobDispatcher()->EnumerateChildren(&walker);
}

static int mwd_thread(void* arg) {
    for (;;) {
        thread_sleep(LK_SEC(1));
        DumpProcessMemoryUsage("MemoryHog! ", mwd_limit);
    }
}

static int cmd_diagnostics(int argc, const cmd_args* argv, uint32_t flags) {
    int rc = 0;

    if (argc < 2) {
    notenoughargs:
        printf("not enough arguments:\n");
    usage:
        printf("%s ps         : list processes\n", argv[0].str);
        printf("%s mwd  <mb>  : memory watchdog\n", argv[0].str);
        printf("%s ht   <pid> : dump process handles\n", argv[0].str);
        printf("%s jb   <pid> : list job tree\n", argv[0].str);
        printf("%s kill <pid> : kill process\n", argv[0].str);
        printf("%s asd  <pid> : dump process address space\n", argv[0].str);
        return -1;
    }

    if (strcmp(argv[1].str, "mwd") == 0) {
        if (argc == 3) {
            mwd_limit = argv[2].u * 256;
        }
        if (!mwd_running) {
            thread_t* t = thread_create("mwd", mwd_thread, nullptr, DEFAULT_PRIORITY, DEFAULT_STACK_SIZE);
            if (t) {
                mwd_running = true;
                thread_resume(t);
            }
        }
    } else if (strcmp(argv[1].str, "ps") == 0) {
        if ((argc == 3) && (strcmp(argv[2].str, "help") == 0)) {
            DumpProcessListKeyMap();
        } else {
            DumpProcessList();
        }
    } else if (strcmp(argv[1].str, "ht") == 0) {
        if (argc < 3)
            goto usage;
        DumpProcessHandles(argv[2].u);
    } else if (strcmp(argv[1].str, "jb") == 0) {
        if (argc < 3)
            goto usage;
        DumpJobTreeForProcess(argv[2].u);
    } else if (strcmp(argv[1].str, "kill") == 0) {
        if (argc < 3)
            goto usage;
        KillProcess(argv[2].u);
    } else if (strcmp(argv[1].str, "asd") == 0) {
        if (argc < 3)
            goto usage;
        DumpProcessAddressSpace(argv[2].u);
    } else {
        printf("unrecognized subcommand\n");
        goto usage;
    }
    return rc;
}

STATIC_COMMAND_START
STATIC_COMMAND("mx", "magenta diagnostics", &cmd_diagnostics)
STATIC_COMMAND_END(mx);
