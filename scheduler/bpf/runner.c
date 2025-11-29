#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <sys/resource.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>

// CONFIGURAÇÕES
#define BPF_OBJ_PATH "kube_sched.bpf.o"
#define MAP_NAME "high_prio_cgroups"
#define OPS_NAME "kube_ops"
#define PIN_PATH "/sys/fs/bpf/high_prio_cgroups"

static volatile bool exiting = false;

/*
 * libbpf_print_fn() - Callback for libbpf debug logging.
 * Useful for diagnosing kernel-side verifier errors.
 */
static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args)
{
    return vfprintf(stderr, format, args);
}

/*
 * sig_handler() - Signal handler for graceful shutdown.
 */
static void sig_handler(int sig)
{
    exiting = true;
}

int main(int argc, char **argv)
{
    struct bpf_object *obj = NULL;
    struct bpf_link *link = NULL;
    struct bpf_map *map = NULL;
    int err;

    // 1. Setup logging and signal handling
    libbpf_set_print(libbpf_print_fn);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    // 2. Open the BPF Object file
    printf("--- Opening %s ---\n", BPF_OBJ_PATH);
    obj = bpf_object__open(BPF_OBJ_PATH);
    if (!obj)
    {
        fprintf(stderr, "ERROR: Failed to open BPF object\n");
        return 1;
    }

    // 3. Load into the Kernel (Verifier Check)
    printf("--- Loading into Kernel ---\n");
    err = bpf_object__load(obj);
    if (err)
    {
        fprintf(stderr, "ERROR: Failed to load BPF object (Check logs above)\n");
        goto cleanup;
    }

    // 4. Pin the BPF Map for the User-Space Agent
    // Unlink any existing pin to ensure a fresh start
    unlink(PIN_PATH);

    map = bpf_object__find_map_by_name(obj, MAP_NAME);
    if (!map)
    {
        fprintf(stderr, "ERROR: Map '%s' not found in BPF object!\n", MAP_NAME);
        goto cleanup;
    }

    printf("--- Pinning map to %s ---\n", PIN_PATH);
    err = bpf_map__pin(map, PIN_PATH);
    if (err)
    {
        fprintf(stderr, "ERROR: Failed to pin map: %s\n", strerror(errno));
        goto cleanup;
    }

    // 5. Activate the Scheduler (Attach Struct Ops)
    map = bpf_object__find_map_by_name(obj, OPS_NAME);
    if (!map)
    {
        fprintf(stderr, "ERROR: Struct Ops '%s' not found!\n", OPS_NAME);
        goto cleanup;
    }

    printf("--- ACTIVATING SCHED_EXT ---\n");
    link = bpf_map__attach_struct_ops(map);
    if (!link)
    {
        fprintf(stderr, "ERROR: Failed to attach scheduler. Kernel supports SCX? Error: %s\n", strerror(errno));
        goto cleanup;
    }

    printf("\n>>> SUCCESS! VANGUARD SCHEDULER IS RUNNING! <<<\n");
    printf("Press Ctrl+C to stop and revert to CFS.\n");

    // Keep the process alive to maintain the scheduler active
    while (!exiting)
    {
        sleep(1);
    }

cleanup:
    printf("\n--- Cleaning up and Exiting ---\n");
    // Unpin the map to avoid stale references
    unlink(PIN_PATH);
    if (link)
        bpf_link__destroy(link);
    if (obj)
        bpf_object__close(obj);
    return err != 0;
}
