import json
import csv
import subprocess
import os
import sys

OUT_CSV = "resultados_EXT_stress.csv"
COUNT = 50
TARGET_IP = "172.18.0.73"
TARGET_PORT = "30001"
JSON_FILE = "temp_result.json"

def run_memtier(run_id):
    print(f">>> [Run {run_id}/{COUNT}] Executando memtier...")
    
    cmd = [
        "memtier_benchmark",
        "-s", TARGET_IP,
        "-p", TARGET_PORT,
        "--protocol=redis",
        "--ratio=1:4",
        "--data-size=128",
        "--test-time=30",
        "--threads=2",
        "--clients=20",
        f"--json-out-file={JSON_FILE}"
    ]
    
    result = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if result.returncode != 0:
        print(f"ERRO no memtier: {result.stderr.decode('utf-8')}")
        return False
    return True

def extract_data(run_id):
    if not os.path.exists(JSON_FILE):
        return None

    with open(JSON_FILE, 'r') as f:
        try:
            data = json.load(f)
        except json.JSONDecodeError:
            return None

    # 1. Achar a raiz
    stats_root = None
    possible_roots = ["ALL STATS", "All Stats", "Run #1"]
    for key in possible_roots:
        if key in data:
            stats_root = data[key]
            break
            
    if stats_root is None:
        print(f"CRÍTICO: Raiz não encontrada. Chaves: {list(data.keys())}")
        return None

    # --- FUNÇÕES INTELIGENTES ---
    def get_val(section, field):
        sec_data = stats_root.get(section) or stats_root.get(section.upper())
        if not sec_data: return 0
        return sec_data.get(field, 0)

    def get_latency(section, target_p):
        sec_data = stats_root.get(section) or stats_root.get(section.upper())
        if not sec_data: return 0

        dist = sec_data.get("Percentile Latencies") or sec_data.get("Latency Distribution")
        if not dist: return 0

        # --- CORREÇÃO DO PREFIXO 'p' ---
        if isinstance(dist, dict):
            for key, val in dist.items():
                # Remove o 'p' se existir (ex: "p50.00" -> "50.00")
                clean_key = key.lower().replace("p", "")
                try:
                    if abs(float(clean_key) - target_p) < 0.01:
                        return val
                except ValueError:
                    continue
                    
        elif isinstance(dist, list):
            for item in dist:
                p = item.get("Percentile", 0)
                if abs(p - target_p) < 0.01:
                    return item.get("Latency", 0)
        return 0

    row = {'run': run_id}
    
    map_sections = {'sets': 'Sets', 'gets': 'Gets', 'tot': 'Totals'}

    for prefix, sec_name in map_sections.items():
        # Campos diretos
        row[f'{prefix}_ops']      = get_val(sec_name, "Ops/sec")
        row[f'{prefix}_hits']     = get_val(sec_name, "Hits/sec")
        row[f'{prefix}_misses']   = get_val(sec_name, "Misses/sec")
        row[f'{prefix}_kb']       = get_val(sec_name, "KB/sec")
        row[f'{prefix}_avg_lat']  = get_val(sec_name, "Average Latency") # Nome corrigido

        # Percentis
        row[f'{prefix}_p50']   = get_latency(sec_name, 50.0)
        row[f'{prefix}_p99']   = get_latency(sec_name, 99.0)
        row[f'{prefix}_p999']  = get_latency(sec_name, 99.9)

    return row

def main():
    fieldnames = ['run']
    for type_ in ['sets', 'gets', 'tot']:
        for metric in ['ops', 'hits', 'misses', 'avg_lat', 'p50', 'p99', 'p999', 'kb']:
            fieldnames.append(f"{type_}_{metric}")

    print(f"Iniciando. Saída: {OUT_CSV}")
    
    with open(OUT_CSV, 'w', newline='') as csvfile:
        writer = csv.DictWriter(csvfile, fieldnames=fieldnames)
        writer.writeheader()

        for i in range(1, COUNT + 1):
            if run_memtier(i):
                row = extract_data(i)
                if row:
                    writer.writerow(row)
                    csvfile.flush()
                    # Mostra o P99 para você confirmar visualmente
                    print(f"   -> Run {i} OK. (Total P99: {row['tot_p99']} ms)")
                else:
                    print(f"   -> Run {i} falhou.")
            
            if os.path.exists(JSON_FILE):
                os.remove(JSON_FILE)

    print("\n✔ Feito.")

if __name__ == "__main__":
    main()
