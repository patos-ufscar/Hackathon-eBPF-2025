# Setting Up The Environment

Of course, for this project we needed an environment to test our code. Namely, an environment with as recent of a kernel version as possible, with all SCHED_EXT features enabled and where we could set up our Kubernetes cluster and run our scheduler.

Since we are part of a research group in our college in partnership with **Magalu Cloud**, we used a VM inside their infrastructure for this purpose. Their Ubuntu VMs by default utilize an older LTS version of the system, which comes with an older kernel version. So we decided to research what would be the simplest way to update the VM's kernel so that we could easily and quickly reproduce this test environment in case something went wrong.

For this purpose, we decided to use [mainline](https://github.com/cappelikan/mainline), which is a tool for installing the latest kernel version on Ubuntu-based distributions. This worked out quite nicely, with the exception of some expected caveats, like missing necessary packages. As we figured out how to set up this VM to have all the necessary tools, we wrote a step-by-step of the entire process. Here it is (translated to english):

> ⚠️ This setup requires two terminals open on the machine.

1. Update the kernel to the most recent available version with mainline:
```sh
sudo apt update
sudo add-apt-repository ppa:cappelikan/ppa
sudo apt update
sudo apt install policykit-1 -y
sudo apt install wireless-regdb -y
sudo apt install mainline -y
sudo apt --fix-broken install -y
sudo reboot
```

2. Install essential libraries:
```sh
sudo apt install clang llvm make pkg-config libbpf-dev linux-headers-$(uname -r) pahole git build-essential libelf-dev zlib1g-dev libssl-dev curl build-essential pkg-config libssl-dev
```

3. Build and install bpftool:
```sh
git clone --recurse-submodules https://github.com/libbpf/bpftool.git
cd bpftool/src
make
sudo make install
hash -r
bpftool version
```

4. Download the repository and dump the vmlinux interface:
```sh
cd ~
git clone https://github.com/patos-ufscar/Hackathon-eBPF-2025.git
cd Hackathon-eBPF-2025/scheduler/bpf/
sudo bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h
```

5. Expose sched_ext headers:
```sh
cd /usr/src
sudo wget https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.17.8.tar.xz
sudo tar -xf linux-6.17.8.tar.xz
export SCX_INCLUDE=/usr/src/linux-6.17.8/tools/sched_ext/include
```
6. Compile main.bpf.c:
```sh
cd ~/Hackathon-eBPF-2025/scheduler/bpf
clang -O2 -g -target bpf -D__TARGET_ARCH_x86 \
      -I"$SCX_INCLUDE" -I. \
      -Wall -Werror \
      -c main.bpf.c -o kube_sched.bpf.o
```

7. Configure Kubernetes
- For this step, we used k3s, a lighweight Kubernetes cluster setup
```sh
curl -sfL https://get.k3s.io | sh -
sudo kubectl apply -f ~/Hackathon-eBPF-2025/manifests/redis.yaml
sudo kubectl apply -f ~/Hackathon-eBPF-2025/manifests/stress-noise.yaml
```

8. Compile and execute runner.c:
```sh
gcc runner.c -o runner -lbpf
```

9. In other tab, build and run main.go (userspace code):
```sh
GOOS=linux GOARCH=amd64 go build -o scheduler-cli
sudo KUBECONFIG=/etc/rancher/k3s/k3s.yaml ./scheduler-cli
# Choose redis container
```
> **NOTE**: kubeconfig file changes depending on environment, this one is for k3s. In minikube, it would be `KUBECONFIG=/etc/kubernetes/admin.conf`