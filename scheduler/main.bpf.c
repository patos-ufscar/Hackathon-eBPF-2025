#include <scx/common.bpf.h>
#include "intf.h"

/*
 mapa quearmazena os pids dos processos que estão
 rodando dentro do container que queremos priorizar.

 Talvez mude a maneira de tratar processos no futuro.
 Não se apegar.
*/
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 10);
    __type(key, u64);   // cgroup id
    __type(value, u64); // ?
} priority_container SEC(".maps");

UEI_DEFINE(uei);

/* Contabiliza prioridade 4:1 */
int count = 0;

/*
 * Number of CPUs running in scheduler
 */
static u64 nr_cpu_ids;

/*
 * Current system vruntime.
 */
static u64 vtime_now;

/*
 * Default time slice.
 */
const volatile u64 slice_ns = 10000ULL;

#define PRIORITY 4

#define NORMAL_PRIORITY 1024
#define HIGH_PRIORITY 4096

s32 BPF_STRUCT_OPS(kube_select_cpu, struct task_struct *p, s32 prev_cpu, u64 wake flags)
{
    scx_bpf_dsq_insert_vtime(p, prev_cpu, slice_ns, p->scx.dsq_vtime, SCX_DSQ_INSERT_TAIL);

    return -1;
}

void BPF_STRUCT_OPS(kube_enqueue, struct task_struct *p, u64 enq_flags)
{
    // pretty random, no?
    cpu = (bpf_get_prandom_u32() % nr_cpus);

    scx_bpf_dsq_insert_vtime(p, cpu, p->scx.dsq_vtime, SCX_DSQ_INSERT_TAIL);
}

void BPF_STRUCT_OPS(kube_dispatch, s32 cpu, struct task_struct *prev)
{
    scx_bpf_dsq_move_to_local(cpu);
}

void BPF_STRUCT_OPS(kube_running, struct task_struct *p)
{
    
}

void BPF_STRUCT_OPS(kube_stopping, struct task_struct *p)
{
    // isso aqui da merda com tempo constante, concertar!
    u64 delta_exec = slice_ns - p->scx.slice;

    // Cgroup ID of current task p
    u64 cgroup_id = bpf_get_current_cgroup_id();
    u32 peso_da_tarefa = NORMAL_PRIORITY;

    if (bpf_map_lookup_elem(&high_prio_cgroups, &cgroup_id) != NULL)
        peso_da_tarefa = HIGH_PRIORITY;

    u64 delta_vruntime = delta_exec * PESO_NORMAL / peso_da_tarefa;
    p->scx.dsq_vtime += delta_vruntime;
}

void BPF_STRUCT_OPS(kube_enable, struct task_struct *p)
{
    p->scx.dsq_vtime = vtime_now;
}

s32 BPF_STRUCT_OPS(kube_init)
{
    int err;

    nr_cpu_ids = scx_bpf_nr_cpu_ids();

    // Create DSQ per CPU
    bpf_for(cpu, 0, nr_cpu_ids) {
        err = scx_bpf_create_dsq(cpu, cpu);
        if (err) {
            scx_bpf_error("Failed to create DSQ for CPU %d: %d", cpu, err);
            return err;
        }
    }

    return 0;
}

void BPF_STRUCT_OPS(kube_exit, struct scx_exit_info *ei)
{
	UEI_RECORD(uei, ei);
}

SCX_OPS_DEFINE(kube_ops,
    .select_cpu		= (void *)kube_select_cpu,  //
    .enqueue		= (void *)kube_enqueue,     //
    .dispatch		= (void *)kube_dispatch,    //
    .running		= (void *)kube_running,
    .stopping		= (void *)kube_stopping,    // ...
    .enable			= (void *)kube_enable,      //
    .init			= (void *)kube_init,        //
    .exit			= (void *)kube_exit,        //
    .name			= "kube");