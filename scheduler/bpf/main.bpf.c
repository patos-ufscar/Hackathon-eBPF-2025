//go:build ignore
#include <scx/common.bpf.h>
#include "intf.h"

char _license[] SEC("license") = "GPL";

/*
 * Constant: SHARED_DSQ_ID
 * Description: ID for the global Dispatch Queue (DSQ).
 * We use a value > 0 to avoid conflicts with built-in DSQs.
 * This implements a global queue model where all CPUs pull from a single source.
 */
#define SHARED_DSQ_ID 1025

/*
 * - NORMAL_PRIORITY: Standard weight for regular tasks (equivalent to nice 0).
 * - HIGH_PRIORITY: Weight for prioritized cgroups. 4x the normal weight
 * means the task accumulates vruntime 4x slower, running 4x more frequently/longer.
 */
#define NORMAL_PRIORITY 1024
#define HIGH_PRIORITY 4096

/*
 * The default time slice assigned to tasks (10ms).
 * This defines the granularity of preemption.
 */
const volatile u64 slice_ns = 10000000ULL;

/*
 * Task-local storage structure.
 * Used to maintain state across scheduler callbacks without acquiring
 * global locks or performing hash map lookups.
 *
 * @last_run_at: Timestamp (ns) when the task started running on the CPU.
 */
struct task_ctx {
    u64 last_run_at;
};

/*
 * Storage for per-task context.
 */
struct {
    __uint(type, BPF_MAP_TYPE_TASK_STORAGE);
    __uint(map_flags, BPF_F_NO_PREALLOC);
    __type(key, int);
    __type(value, struct task_ctx);
} task_ctx_stor SEC(".maps");

/*
 * Registry of Cgroup IDs that require high-priority scheduling.
 * Populated by the user-space agent.
 * Key: Cgroup ID (u64) | Value: empty
 */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 100);
    __type(key, u64);
    __type(value, u64);
} high_prio_cgroups SEC(".maps");

// Global state for system virtual time
static u64 vtime_now;

UEI_DEFINE(uei);

/*
 * Returns a pointer to the task_ctx stored in local storage.
 * Creates a new entry if one does not exist.
 */
static __always_inline struct task_ctx *lookup_task_ctx(struct task_struct *p)
{
    return bpf_task_storage_get(&task_ctx_stor, p, 0, BPF_LOCAL_STORAGE_GET_F_CREATE);
}

/*
 * Returns the CPU ID where the task should run.
 * Current policy: Simply returns @prev_cpu. This is not a true
 * cache-affinity policy, but in practice it can preserve some locality.
 */
s32 BPF_STRUCT_OPS(kube_select_cpu, struct task_struct *p, s32 prev_cpu, u64 wake_flags)
{
    return prev_cpu;
}

/*
 * Places the task into the global SHARED_DSQ_ID.
 * The task is ordered within the DSQ based on its vruntime (p->scx.dsq_vtime).
 */
void BPF_STRUCT_OPS(kube_enqueue, struct task_struct *p, u64 enq_flags)
{
    scx_bpf_dsq_insert_vtime(p, SHARED_DSQ_ID, slice_ns, p->scx.dsq_vtime, 0);
}

/*
 * Attempts to consume a task from the global SHARED_DSQ_ID and move it
 * to the local DSQ for immediate execution.
 */
void BPF_STRUCT_OPS(kube_dispatch, s32 cpu, struct task_struct *prev)
{
    scx_bpf_dsq_move_to_local(SHARED_DSQ_ID);
}

/*
 * Records the start timestamp to calculate wall-clock execution time later.
 * Updates the global monotonic vtime if the task's vtime is ahead.
 */
void BPF_STRUCT_OPS(kube_running, struct task_struct *p)
{
    struct task_ctx *tctx = lookup_task_ctx(p);
    if (!tctx) return;

    tctx->last_run_at = bpf_ktime_get_ns();
    
    if (time_before(vtime_now, p->scx.dsq_vtime))
        vtime_now = p->scx.dsq_vtime;
}

/*
 * Computes wall-clock execution time and updates the task’s virtual
 * runtime using cgroup-based weights. Higher weights slow vtime growth,
 * letting such tasks appear earlier in the timeline. The approach is
 * conceptually similar to CFS weighting, but implemented with a simpler
 * vtime model.
 */
void BPF_STRUCT_OPS(kube_stopping, struct task_struct *p, bool runnable)
{
    struct task_ctx *tctx = lookup_task_ctx(p);
    if (!tctx) return;

    u64 now = bpf_ktime_get_ns();
    // task real time running in cpu
    u64 delta_exec = now - tctx->last_run_at;

    if ((s64)delta_exec < 0) delta_exec = 0;

    // Priority determination
    u64 cgroup_id = bpf_get_current_cgroup_id();
    u32 task_weight = NORMAL_PRIORITY;

    if (bpf_map_lookup_elem(&high_prio_cgroups, &cgroup_id) != NULL)
        task_weight = HIGH_PRIORITY;

    // vruntime calculation:
    // Tasks with higher weight accumulate vtime slower, appearing "earlier"
    // in the virtual timeline, thus getting scheduled sooner.
    u64 delta_vruntime = delta_exec * NORMAL_PRIORITY / task_weight;

    p->scx.dsq_vtime += delta_vruntime;
}

/*
 * Prevents "lagging" tasks (tasks that slept for a long time) from
 * accumulating excessive credit and starving others upon waking.
 * Clamps the vtime to a maximum lag of one slice.
 */
void BPF_STRUCT_OPS(kube_enable, struct task_struct *p)
{
    if (p->scx.dsq_vtime < vtime_now - slice_ns)
        p->scx.dsq_vtime = vtime_now - slice_ns;
}

/*
 * Initializes global state and creates the shared dispatch queue.
 */
s32 BPF_STRUCT_OPS_SLEEPABLE(kube_init)
{
    return scx_bpf_create_dsq(SHARED_DSQ_ID, -1);
}

/*
 * Dumps debug information if the scheduler exits abnormally.
 */
void BPF_STRUCT_OPS(kube_exit, struct scx_exit_info *ei)
{
	UEI_RECORD(uei, ei);
}

SCX_OPS_DEFINE(kube_ops,
    .select_cpu		= (void *)kube_select_cpu,
    .enqueue		= (void *)kube_enqueue,
    .dispatch		= (void *)kube_dispatch,
    .running		= (void *)kube_running,
    .stopping		= (void *)kube_stopping,
    .enable			= (void *)kube_enable,
    .init			= (void *)kube_init,
    .exit			= (void *)kube_exit,
    .name			= "kube");