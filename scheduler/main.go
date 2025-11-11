package main

import (
	"context"
	"fmt"
	"log"
	"os"
	"strings"
	"syscall"

	v1 "k8s.io/api/core/v1"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/client-go/kubernetes"
	"k8s.io/client-go/tools/clientcmd"

	"github.com/AlecAivazis/survey/v2"
	"github.com/containerd/containerd"
	_ "github.com/containerd/containerd/api/services/tasks/v1"

	"github.com/cilium/ebpf"
)

// BPF map to store the PIDs that will be prioritized
// Kernelspace code will create this
// for testing use
// sudo bpftool map create /sys/fs/bpf/priority_pids type hash key 4 value 4 entries 1024 name priority_pids
// to create the map
// sudo bpftool map dump pinned /sys/fs/bpf/priority_pids
// to inspect the map
const bpfMapPath = "/sys/fs/bpf/priority_pids"

const containerdSocket = "/run/containerd/containerd.sock"

type KubeContainer struct {
	PodName       string
	ContainerName string
	ContainerID   string
}

func main() {
	ctx := context.Background()
	clientset, err := getKubeClient()
	if err != nil {
		log.Fatalf("Failed to create Kubernetes clientset: %v", err)
	}

	/* nodeName := os.Getenv("NODE_NAME")
	if nodeName == "" {
		nodeName, _ = os.Hostname()
		log.Printf("NODE_NAME env var not set, falling back to hostname: %s", nodeName)
	} */

	// For single-node testing
	nodeName := "minikube"

	runningContainers, err := getLocalContainers(ctx, clientset, nodeName)
	if err != nil {
		log.Fatalf("Failed to retrieve containers: %v", err)
	}
	if len(runningContainers) <= 0 {
		log.Fatalf("No containers running in this node")
	}

	selectedContainer, err := selectContainer(runningContainers)
	if err != nil {
		log.Fatalf("Failed to select container: %v", err)
	}

	log.Printf("You selected: Pod=%s, Container=%s", selectedContainer.PodName, selectedContainer.ContainerName)

	// Containerd setup
	/* log.Printf("Connecting to containerd to find PID")
	pid, err := getPIDFromContainerd(ctx, selectedContainer.ContainerID) */

	// Docker setup
	log.Printf("Connecting to docker to find PID")
	pid, err := getPIDFromDocker(ctx, selectedContainer.ContainerID)
	if err != nil {
		log.Fatalf("Failed to get PID: %v", err)
	}
	log.Printf("Found host PID: %d", pid)

	path := "/proc/" + fmt.Sprint(pid) + "/cgroup"
	f, err := os.ReadFile(path)
	if err != nil {
		log.Fatalf("Failed to get cgroup: %v", err)
	}
	cgroupRaw := strings.TrimSpace(string(f))
	cgroup := ""

	for _, line := range strings.Split(cgroupRaw, "\n") {
		if strings.HasPrefix(line, "0::/") {
			cgroup = line // Found the v2 line
			break
		}
	}

	if cgroup == "" {
		log.Fatalf("Could not find cgroup v2 path (0::/...) for PID %d. Found: %s", pid, cgroupRaw)
	}

	log.Printf("Writing PID %d (Cgroup: %s) to eBPF map at %s", pid, cgroup, bpfMapPath)
	if err := writePIDToMap(pid, cgroup); err != nil {
		log.Fatalf("Failed to write to eBPF map: %v", err)
	}

	log.Println("Success! The eBPF scheduler has been notified.")
	log.Printf("PID %d will now be prioritized.", pid)
}

func getKubeClient() (*kubernetes.Clientset, error) {
	loadingRules := clientcmd.NewDefaultClientConfigLoadingRules()
	configOverrides := &clientcmd.ConfigOverrides{}
	config, err := clientcmd.NewNonInteractiveDeferredLoadingClientConfig(loadingRules, configOverrides).ClientConfig()
	if err != nil {
		return nil, fmt.Errorf("building client config: %w", err)
	}

	return kubernetes.NewForConfig(config)
}

func getLocalContainers(ctx context.Context, cs *kubernetes.Clientset, nodeName string) ([]KubeContainer, error) {
	podList, err := cs.CoreV1().Pods("").List(ctx, metav1.ListOptions{
		FieldSelector: "spec.nodeName=" + nodeName,
	})

	if err != nil {
		return nil, fmt.Errorf("listing pods: %w", err)
	}

	var containers []KubeContainer
	for _, pod := range podList.Items {
		if pod.Status.Phase != v1.PodRunning {
			continue
		}
		for _, status := range pod.Status.ContainerStatuses {
			if status.State.Running == nil {
				continue
			}

			containers = append(containers, KubeContainer{
				PodName:       pod.Name,
				ContainerName: status.Name,
				ContainerID:   status.ContainerID,
			})
		}
	}

	return containers, nil
}

func selectContainer(runningContainers []KubeContainer) (KubeContainer, error) {
	options := make([]string, len(runningContainers))
	containerMap := make(map[string]KubeContainer)

	for i, c := range runningContainers {
		display := fmt.Sprintf("Pod: %s / Container %s", c.PodName, c.ContainerName)
		options[i] = display
		containerMap[display] = c
	}

	var selectedDisplay string
	prompt := &survey.Select{
		Message:  "Choose a container to prioritize:",
		Options:  options,
		PageSize: 15,
	}
	if err := survey.AskOne(prompt, &selectedDisplay, survey.WithStdio(os.Stdin, os.Stdout, os.Stderr)); err != nil {
		return KubeContainer{}, fmt.Errorf("survey prompt failed: %w", err)
	}

	return containerMap[selectedDisplay], nil
}

// Function for kubernetes setup using containerd
func getPIDFromContainerd(ctx context.Context, containerId string) (uint32, error) {
	id := strings.TrimPrefix(containerId, "containerd://")
	client, err := containerd.New(containerdSocket, containerd.WithDefaultNamespace("k8s.io"))
	if err != nil {
		return 0, fmt.Errorf("connecting to containerd: %w", err)
	}
	defer client.Close()

	container, err := client.LoadContainer(ctx, id)
	if err != nil {
		return 0, fmt.Errorf("loading container %s: %w", id, err)
	}

	task, err := container.Task(ctx, nil)
	if err != nil {
		return 0, fmt.Errorf("getting task for container %s: %w", id, err)
	}

	_, err = task.Status(ctx)
	if err != nil {
		return 0, fmt.Errorf("getting task status for container %s: %w", id, err)
	}

	return task.Pid(), nil
}

// Function for minikube setup using docker
func getPIDFromDocker(ctx context.Context, containerId string) (uint32, error) {
	id := strings.TrimPrefix(containerId, "docker://")
	client, err := containerd.New(containerdSocket, containerd.WithDefaultNamespace("moby"))
	if err != nil {
		return 0, fmt.Errorf("connecting to containerd: %w", err)
	}
	defer client.Close()

	container, err := client.LoadContainer(ctx, id)
	if err != nil {
		return 0, fmt.Errorf("loading container %s: %w", id, err)
	}

	task, err := container.Task(ctx, nil)
	if err != nil {
		return 0, fmt.Errorf("getting task for container %s: %w", id, err)
	}

	return task.Pid(), nil
}

func getCgroupID(cgroupRaw string) (uint64, error) {
	parts := strings.SplitN(cgroupRaw, "::", 2)
	if len(parts) != 2 {
		return 0, fmt.Errorf("invalid cgroup format: %s", cgroupRaw)
	}
	cgroupPath := parts[1]

	const cgroupRoot = "/sys/fs/cgroup"
	fullPath := cgroupRoot + cgroupPath

	var stat syscall.Stat_t
	if err := syscall.Stat(fullPath, &stat); err != nil {
		return 0, fmt.Errorf("could not stat cgroup path %s: %w", fullPath, err)
	}

	return stat.Ino, nil
}

func writePIDToMap(pid uint32, cgroup string) error {
	cgroupId, err := getCgroupID(cgroup)
	if err != nil {
		return fmt.Errorf("failed to get cgroup ID: %w", err)
	}

	fmt.Printf("Writing to map: PID %d -> CgroupID %d\n", pid, cgroupId)

	priorityMap, err := ebpf.LoadPinnedMap(bpfMapPath, nil)
	if err != nil {
		return fmt.Errorf("loading pinned map: %w. Is the eBPF program running?", err)
	}
	defer priorityMap.Close()

	if err := priorityMap.Put(pid, cgroupId); err != nil {
		return fmt.Errorf("putting PID in map: %w", err)
	}

	return nil
}
