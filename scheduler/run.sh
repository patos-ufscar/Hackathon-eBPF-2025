if [ "$1" == "compile" ]; then
    go build -o scheduler-cli
    exit 0
fi

if [ "$1" == "test-vm" ]; then
    GOOS=linux GOARCH=amd64 go build -o scheduler-cli
    minikube cp ./scheduler-cli /home/docker/scheduler-cli
    minikube ssh "sudo chmod +x /home/docker/scheduler-cli"
    minikube ssh "sudo bpftool map create /sys/fs/bpf/priority_pids type hash key 4 value 8 entries 256 name cgroup_priorities" 
    minikube ssh "sudo KUBECONFIG=/etc/kubernetes/admin.conf ./scheduler-cli"
    minikube ssh "sudo bpftool map dump pinned /sys/fs/bpf/priority_pids"
    minikube ssh "sudo rm /sys/fs/bpf/priority_pids"
    exit 0
fi

sudo mkdir -p /root/.kube
sudo ln -s $HOME/.kube/config /root/.kube/config
sudo ./scheduler-cli