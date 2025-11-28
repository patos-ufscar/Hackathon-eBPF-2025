# Tutorial

This guide assumes that SCX_MUS is already installed and running on a target machine.

To run the benchmark from a second machine with network access to the target:

1. Clone the repository
```sh
cd ~
git clone https://github.com/patos-ufscar/Hackathon-eBPF-2025.git
cd Hackathon-eBPF-2025/evaluation/benchmark
```

2. Install memtier_benchmark
```sh
sudo apt-get install memtier-benchmark
```

3. Configure the benchmark script

Edit benchmark.py and set the target machine IP, the output CSV name and the number of tests that you would like to do:
```sh
OUT_CSV = "your_output.csv"
COUNT = 50
TARGET_IP = "TARGET_MACHINE_IP"
TARGET_PORT = "30001"
JSON_FILE = "temp_result.json"
```

4. Execute the script
```sh
python3 benchmark.py
```