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
#define MAP_NAME     "high_prio_cgroups"
#define OPS_NAME     "kube_ops"
#define PIN_PATH     "/sys/fs/bpf/priority_pids"

static volatile bool exiting = false;

// Função para imprimir logs detalhados da libbpf (útil se der erro no kernel)
static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args) {
    return vfprintf(stderr, format, args);
}

static void sig_handler(int sig) {
    exiting = true;
}

int main(int argc, char **argv) {
    struct bpf_object *obj = NULL;
    struct bpf_link *link = NULL;
    struct bpf_map *map = NULL;
    int err;

    // 1. Configura Logs e Sinais
    libbpf_set_print(libbpf_print_fn);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    // 2. Abre o arquivo BPF (O seu .o compilado)
    printf("--- Abrindo %s ---\n", BPF_OBJ_PATH);
    obj = bpf_object__open(BPF_OBJ_PATH);
    if (!obj) {
        fprintf(stderr, "ERRO: Falha ao abrir objeto BPF\n");
        return 1;
    }

    // 3. Carrega no Kernel (Verifier checa o código aqui)
    printf("--- Carregando no Kernel ---\n");
    err = bpf_object__load(obj);
    if (err) {
        fprintf(stderr, "ERRO: Falha ao carregar (Verifique logs acima)\n");
        goto cleanup;
    }

    // 4. Pina o Mapa para o Go usar
    // Primeiro, limpamos qualquer pin antigo
    unlink(PIN_PATH);

    map = bpf_object__find_map_by_name(obj, MAP_NAME);
    if (!map) {
        fprintf(stderr, "ERRO: Mapa '%s' nao encontrado!\n", MAP_NAME);
        goto cleanup;
    }

    printf("--- Pinando mapa em %s ---\n", PIN_PATH);
    err = bpf_map__pin(map, PIN_PATH);
    if (err) {
        fprintf(stderr, "ERRO: Falha ao pinar mapa: %s\n", strerror(errno));
        goto cleanup;
    }

    // 5. Ativa o Escalonador (Attach Struct Ops)
    map = bpf_object__find_map_by_name(obj, OPS_NAME);
    if (!map) {
        fprintf(stderr, "ERRO: Struct Ops '%s' nao encontrada!\n", OPS_NAME);
        goto cleanup;
    }

    printf("--- ATIVANDO SCHED_EXT ---\n");
    link = bpf_map__attach_struct_ops(map);
    if (!link) {
        fprintf(stderr, "ERRO: Falha ao anexar scheduler. Kernel suporta SCX? Erro: %s\n", strerror(errno));
        goto cleanup;
    }

    printf("\n>>> SUCESSO! SEU ESCALONADOR ESTA RODANDO! <<<\n");
    printf("Use Ctrl+C para encerrar e voltar ao CFS.\n");

    // Loop para manter o programa vivo
    while (!exiting) {
        sleep(1);
    }

cleanup:
    printf("\n--- Limpando e Saindo ---\n");
    // Despina o mapa para não deixar lixo
    unlink(PIN_PATH);
    if (link) bpf_link__destroy(link);
    if (obj) bpf_object__close(obj);
    return err != 0;
}
