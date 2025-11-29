if [ "$1" == "compile" ]; then
    go build -o scheduler-cli
    exit 0
fi
# automate testing with minikube
if [ "$1" == "test-vm" ]; then
    GOOS=linux GOARCH=amd64 go build -o scheduler-cli
    minikube cp ./scheduler-cli /home/docker/scheduler-cli
    minikube ssh "sudo chmod +x /home/docker/scheduler-cli"
    minikube ssh "sudo bpftool map create /sys/fs/bpf/high_prio_cgroups type hash key 8 value 8 entries 256 name cgroup_priorities" 
    minikube ssh "sudo KUBECONFIG=/etc/kubernetes/admin.conf ./scheduler-cli"
    minikube ssh "sudo bpftool map dump pinned /sys/fs/bpf/high_prio_cgroups"
    minikube ssh "sudo rm /sys/fs/bpf/high_prio_cgroups"
    exit 0
fi

GOOS=linux GOARCH=amd64 go build -o scheduler-cli
sudo KUBECONFIG=/etc/rancher/k3s/k3s.yaml ./scheduler-cli
