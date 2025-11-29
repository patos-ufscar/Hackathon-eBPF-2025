# Intro

In this write-up, we wish to elaborate a bit on the inner workings of SCX_MUS. Specifically, we will focus on the kernel-space program `scheduler/bpf/main.bpf.c`

First, for those who do not know, sched_ext is a kernel framework that allows us to write a custom scheduler and load it, effectively replacing the CFS (Completely Fair Scheduler) with our SCX_MUS for a set of tasks. SCX_MUS is designed to be a Weighted Virtual Time Scheduler (similar to CFS) with a specific focus on container prioritization. It uses eBPF maps to track high-priority cgroups and adjusts their execution time accounting to ensure latency-sensitive applications get CPU time faster than background noise.

# Architecture Overview

SCX_MUS employs a Global Dispatch Queue (DSQ) model. Unlike per-CPU scheduling queues where tasks are strictly bound to specific processors, a global DSQ allows any available CPU to pull tasks from a single shared queue.

The core algorithm is Weighted Virtual Time. Every task has a virtual timestamp (vtime). Tasks with lower vtime are considered to have received less CPU time and are thus prioritized. The 'unfairness' is achieved by manipulating how fast this vtime grows for specific processes, namely, the processes tied to the prioritized container's cgroup ID.

# Core Data Structures

The scheduler relies on two primary eBPF maps to manage state:

`task_ctx_stor` - Task Local Storage
This is a `BPF_MAP_TYPE_TASK_STORAGE` map. It allows us to attach custom data to every task in the system efficiently. We use this to store the `last_run_at` timestamp, which we then use to calculate exactly how long a task ran on the CPU.

`high_prio_cgroups` - Hash Map
This is a `BPF_MAP_TYPE_HASH` map. This serves as the communication channel between user-space and kernel-space. The Go agent writes a cgroup ID of the target container here. The scheduler checks this map, and if a task's cgroup ID is present, it gets a priority boost.

# Configurations & Constants

The first constant is the shared DSQ ID. We give it a random ID to avoid conflicting with existing built-in DSQs.
```C
#define SHARED_DSQ_ID 1025
```

The scheduler also defines specific weights to control the degree of unfairness:
```C
#define NORMAL_PRIORITY 1024
#define HIGH_PRIORITY 4096
```
- NORMAL_PRIORITY: The baseline weight for standard tasks.
- HIGH_PRIORITY: The weight for prioritized containers.
Prioritized tasks will accumulate virtual time 4 times slower than normal tasks. This effectively gives them 4 times the CPU bandwith allocation relative to their peers

# The Scheduling

The logic is implemented via `struct_ops` callbacks that the kernel executes at specific scheduling events. 
```C
SCX_OPS_DEFINE(mus_ops,
    .select_cpu		= (void *)mus_select_cpu,
    .enqueue		= (void *)mus_enqueue,
    .dispatch		= (void *)mus_dispatch,
    .running		= (void *)mus_running,
    .stopping		= (void *)mus_stopping,
    .enable			= (void *)mus_enable,
    .init			= (void *)mus_init,
    .exit			= (void *)mus_exit,
    .name			= "mus");
```

Following are our implementations:

1. Initialization - `mus_init`
```C
s32 BPF_STRUCT_OPS_SLEEPABLE(mus_init)
{
    return scx_bpf_create_dsq(SHARED_DSQ_ID, -1);
}
```
When the scheduler loads, mus_init creates the global dispatch queue using scx_bpf_create_dsq. We use our custom ID to identify this global queue.

2. Selecting a CPU - `mus_select_cpu`
```C
s32 BPF_STRUCT_OPS(mus_select_cpu, struct task_struct *p, s32 prev_cpu, u64 wake_flags)
{
    return prev_cpu;
}
```
When a task wakes up, the kernel asks where to put it. We simply return prev_cpu. This is a simple heuristic that in practise maintain cache locality sometimes. We don't do complex load balancing logic here because the global DSQ handles work distribution naturally (idle CPUs pull from the global queue).

3. Enqueuing Tasks - `mus_enqueue`
```C
void BPF_STRUCT_OPS(mus_enqueue, struct task_struct *p, u64 enq_flags)
{
    scx_bpf_dsq_insert_vtime(p, SHARED_DSQ_ID, slice_ns, p->scx.dsq_vtime, 0);
}
```
When a task is ready to run, it is inserted into the global DSQ. Crucially, we use `scx_bpf_dsq_insert_vtime`. This orders the queue based on `p->scx.dsq_vtime`. Tasks with the lowest vtime are placed at the front.

4. Dispatching - `mus_dispatch`
```C
void BPF_STRUCT_OPS(mus_dispatch, s32 cpu, struct task_struct *prev)
{
    scx_bpf_dsq_move_to_local(SHARED_DSQ_ID);
}
```
When a CPU is idle and looking for work, the CPU calls `scx_bpf_dsq_move_to_local` to pull tasks from the global DSQ into it's own local execution queue. This is how work is distributed across the the nodes' cores.

5. Task Execution Start - `mus_running`
```C
void BPF_STRUCT_OPS(mus_running, struct task_struct *p)
{
    struct task_ctx *tctx = lookup_task_ctx(p);
    if (!tctx) return;

    tctx->last_run_at = bpf_ktime_get_ns();
    
    if (time_before(vtime_now, p->scx.dsq_vtime))
        vtime_now = p->scx.dsq_vtime;
}
```
Just before a task starts executing instructions, we capture the current timestamp with `bpf_ktime_get_ns` and save it in the task's local storage at `last_run_at`. This marks the start of the CPU time of the task.

6. Task Execution Stop - `mus_stopping`
```C
void BPF_STRUCT_OPS(mus_stopping, struct task_struct *p, bool runnable)
{
    struct task_ctx *tctx = lookup_task_ctx(p);
    if (!tctx) return;

    u64 now = bpf_ktime_get_ns();
    u64 delta_exec = now - tctx->last_run_at;

    if ((s64)delta_exec < 0) delta_exec = 0;

    u64 cgroup_id = bpf_get_current_cgroup_id();
    u32 task_weight = NORMAL_PRIORITY;

    if (bpf_map_lookup_elem(&high_prio_cgroups, &cgroup_id) != NULL)
        task_weight = HIGH_PRIORITY;

    u64 delta_vruntime = delta_exec * NORMAL_PRIORITY / task_weight;

    p->scx.dsq_vtime += delta_vruntime;
}
```
This is the most critical function. It runs when a task stops running (either it yielded, slept, or was preempted). We must calculate how much virtual time it consumed. So we check `high_prio_cgroups` to see if the current task belongs to the chosen Kubernetes container. Then we assing task_weight based on the constants we defined above.
Because the scheduler picks the task with the lowest vtime, the prioritized task appears to have barely used the CPU, so the scheduler picks it again immediately, bypassing other waiting tasks.

7. Preventing Starvation - `mus_enable`
```C
void BPF_STRUCT_OPS(mus_enable, struct task_struct *p)
{
    if (p->scx.dsq_vtime < vtime_now - slice_ns)
        p->scx.dsq_vtime = vtime_now - slice_ns;
}
```
When a task wakes up after sleeping for a long time, its vtime might be very old. To prevent it from monopolizing the CPU, we clamp its vtime forward. This ensures a "lagging" task gets a fair start relative to the current system time, rather than an unfair advantage

8. Scheduler Exit - `mus_exit`
```C
void BPF_STRUCT_OPS(mus_exit, struct scx_exit_info *ei)
{
	UEI_RECORD(uei, ei);
}
```
This callback is the final operation invoked when the scheduler is unloaded or disabled. The function calls `UEI_RECORD` (UEI stands for User Exit Info) which is a helper macro provided by the `sched_ext `framework to capture and export debugging information. If the scheduler crashes or is evicted by the kernel watchdog, `UEI_RECORD` ensures that a specific exit reason and context are recorded. This is a default behavior and function for exiting in sched_ext schedulers.
