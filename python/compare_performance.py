import json
import sys
import numpy as np

def extract(records, key, subkey):
    """Extract values from nested JSON structure - from pidplot.py"""
    return [item[key][0][subkey] for item in records]

def read_json(filename):
    """Read JSON file and extract statistics - from pidplot.py"""
    with open(filename, "r") as f:
        entry = json.load(f)
        list = entry['sysstat']['hosts'][0]['statistics']
        return list

def analyze_performance(filename, label):
    """Analyze performance metrics from pidstat JSON"""
    records = read_json(filename)
    
    # Extract metrics using pidplot.py's approach
    cpu_usr = extract(records, 'task-cpu-load', 'usr')
    cpu_system = extract(records, 'task-cpu-load', 'system')
    cpu_total = [usr + sys for usr, sys in zip(cpu_usr, cpu_system)]
    mem_usage = extract(records, 'task-memory', 'MEM')
    context_switches = extract(records, 'context-switch', 'cswch/s')
    
    # Calculate statistics
    stats = {
        'label': label,
        'samples': len(records),
        'cpu_usr_avg': np.mean(cpu_usr),
        'cpu_usr_max': np.max(cpu_usr),
        'cpu_system_avg': np.mean(cpu_system),
        'cpu_system_max': np.max(cpu_system),
        'cpu_total_avg': np.mean(cpu_total),
        'cpu_total_max': np.max(cpu_total),
        'mem_avg': np.mean(mem_usage),
        'mem_max': np.max(mem_usage),
        'context_switches_avg': np.mean(context_switches),
        'context_switches_max': np.max(context_switches)
    }
    
    return stats

def compare_performance(zerocopy_file, regular_file):
    """Compare performance between two pidstat files"""
    
    print("=== Snapcast Performance Comparison ===\n")
    print(f"ZeroCopy file: {zerocopy_file}")
    print(f"Regular file: {regular_file}\n")
    
    # Analyze both files
    stats_zc = analyze_performance(zerocopy_file, "ZeroCopy") 
    stats_reg = analyze_performance(regular_file, "Regular")
    
    # Print comparison
    print(f"{'Metric':<25} {'ZeroCopy':<15} {'Regular':<15} {'Difference':<15} {'% Change'}")
    print("-" * 80)
    
    metrics = [
        ('CPU User %', 'cpu_usr_avg'),
        ('CPU System %', 'cpu_system_avg'), 
        ('CPU Total %', 'cpu_total_avg'),
        ('Memory %', 'mem_avg'),
        ('Context Switches/s', 'context_switches_avg')
    ]
    
    improvements = []
    
    for label, key in metrics:
        zc_val = stats_zc[key]
        reg_val = stats_reg[key]
        diff = zc_val - reg_val
        pct_change = (diff / reg_val * 100) if reg_val != 0 else 0
        
        print(f"{label:<25} {zc_val:<15.2f} {reg_val:<15.2f} {diff:<15.2f} {pct_change:>6.1f}%")
        
        # Track improvements (negative differences are good for CPU/memory)
        if key.startswith('cpu_') or key == 'mem_avg':
            improvements.append((label, pct_change))
    
    print("\n=== Summary ===")
    print(f"Samples: ZeroCopy={stats_zc['samples']}, Regular={stats_reg['samples']}")
    
    print("\nKey Differences:")
    for label, pct in improvements:
        if pct < -1:  # Improvement
            print(f"  ✓ {label}: {abs(pct):.1f}% BETTER (lower)")
        elif pct > 1:  # Regression
            print(f"  ✗ {label}: {pct:.1f}% WORSE (higher)")
        else:
            print(f"  ~ {label}: {pct:.1f}% (negligible)")
    
    # Overall assessment  
    cpu_improvement = (stats_reg['cpu_total_avg'] - stats_zc['cpu_total_avg']) / stats_reg['cpu_total_avg'] * 100
    mem_improvement = (stats_reg['mem_avg'] - stats_zc['mem_avg']) / stats_reg['mem_avg'] * 100
    
    print(f"\nOverall Assessment:")
    print(f"  CPU efficiency: {cpu_improvement:+.1f}% ({'BETTER' if cpu_improvement > 0 else 'WORSE'})")
    print(f"  Memory efficiency: {mem_improvement:+.1f}% ({'BETTER' if mem_improvement > 0 else 'WORSE'})")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python3 compare_performance.py <zerocopy.json> <regular.json>")
        print("Example: python3 compare_performance.py cpu-zc.json cpu-without.json")
        sys.exit(1)
    
    compare_performance(sys.argv[1], sys.argv[2])