#!/usr/bin/env python3

"""
Compare heaptrack data between zerocopy and regular modes
"""

import subprocess
import sys
import re

def parse_heaptrack_summary(filename):
    """Parse heaptrack summary data"""
    try:
        result = subprocess.run(['heaptrack_print', filename], 
                              capture_output=True, text=True, timeout=30)
        
        if result.returncode != 0:
            print(f"Error running heaptrack_print on {filename}")
            return None
        
        output = result.stdout
        
        # Extract key metrics
        stats = {
            'filename': filename,
            'total_calls': 0,
            'peak_consumption': 0,
            'main_allocator_calls': 0,
            'string_allocator_calls': 0
        }
        
        # Look for the main allocation numbers
        lines = output.split('\n')
        for i, line in enumerate(lines):
            # Find "X calls to allocation functions"
            if 'calls to allocation functions' in line and 'std::__new_allocator' in lines[i+1]:
                numbers = re.findall(r'(\d+)\s+calls.*with\s+([0-9.]+[KMG]?B?)\s+peak', line)
                if numbers:
                    calls, peak = numbers[0]
                    stats['main_allocator_calls'] = int(calls)
                    # Convert peak to bytes
                    peak_str = peak.replace('B', '')
                    if 'K' in peak_str:
                        stats['peak_consumption'] = float(peak_str.replace('K', '')) * 1024
                    elif 'M' in peak_str:
                        stats['peak_consumption'] = float(peak_str.replace('M', '')) * 1024 * 1024
                    else:
                        stats['peak_consumption'] = float(peak_str)
            
            # Find string allocations
            elif 'basic_string' in line and 'calls with' in line:
                numbers = re.findall(r'(\d+)\s+calls', line)
                if numbers:
                    stats['string_allocator_calls'] = int(numbers[0])
                    break  # Take first occurrence
        
        # Also look for boost::asio allocations (for regular mode)
        for i, line in enumerate(lines):
            if 'calls to allocation functions' in line and 'boost::asio::aligned_new' in lines[i+1]:
                numbers = re.findall(r'(\d+)\s+calls.*with\s+([0-9.]+[KMG]?B?)\s+peak', line)
                if numbers:
                    calls, peak = numbers[0]
                    if stats['main_allocator_calls'] == 0:  # Only if we didn't find std::allocator
                        stats['main_allocator_calls'] = int(calls)
                        peak_str = peak.replace('B', '')
                        if 'K' in peak_str:
                            stats['peak_consumption'] = float(peak_str.replace('K', '')) * 1024
                        elif 'M' in peak_str:
                            stats['peak_consumption'] = float(peak_str.replace('M', '')) * 1024 * 1024
                        else:
                            stats['peak_consumption'] = float(peak_str)
        
        return stats
        
    except subprocess.TimeoutExpired:
        print(f"Timeout processing {filename}")
        return None
    except Exception as e:
        print(f"Error processing {filename}: {e}")
        return None

def main():
    if len(sys.argv) != 3:
        print("Usage: compare_heaptrack.py <zerocopy.zst> <regular.zst>")
        sys.exit(1)
    
    zc_file = sys.argv[1]
    reg_file = sys.argv[2]
    
    print("=== Heaptrack Memory Analysis Comparison ===\n")
    
    # Parse both files
    zc_stats = parse_heaptrack_summary(zc_file)
    reg_stats = parse_heaptrack_summary(reg_file)
    
    if not zc_stats or not reg_stats:
        print("Failed to parse one or both files")
        sys.exit(1)
    
    print(f"ZeroCopy file: {zc_file}")
    print(f"Regular file: {reg_file}\n")
    
    # Compare metrics
    metrics = [
        ('Main Allocator Calls', 'main_allocator_calls'),
        ('Peak Memory (bytes)', 'peak_consumption'),
        ('String Allocator Calls', 'string_allocator_calls')
    ]
    
    print(f"{'Metric':<25} {'ZeroCopy':<15} {'Regular':<15} {'Difference':<15} {'% Change'}")
    print("-" * 80)
    
    for label, key in metrics:
        zc_val = zc_stats[key]
        reg_val = reg_stats[key]
        diff = zc_val - reg_val
        pct_change = (diff / reg_val * 100) if reg_val != 0 else 0
        
        # Format large numbers
        if key == 'peak_consumption':
            if zc_val >= 1024:
                zc_str = f"{zc_val/1024:.1f}K"
            else:
                zc_str = f"{zc_val:.0f}B"
            if reg_val >= 1024:
                reg_str = f"{reg_val/1024:.1f}K"
            else:
                reg_str = f"{reg_val:.0f}B"
            diff_str = f"{diff:+.0f}B"
        else:
            zc_str = f"{zc_val:,}"
            reg_str = f"{reg_val:,}"
            diff_str = f"{diff:+,}"
        
        print(f"{label:<25} {zc_str:<15} {reg_str:<15} {diff_str:<15} {pct_change:>6.1f}%")
    
    print("\n=== Memory Allocation Analysis ===")
    
    # Calculate improvement percentages
    call_improvement = (reg_stats['main_allocator_calls'] - zc_stats['main_allocator_calls']) / reg_stats['main_allocator_calls'] * 100
    memory_improvement = (reg_stats['peak_consumption'] - zc_stats['peak_consumption']) / reg_stats['peak_consumption'] * 100
    
    print(f"Allocation call reduction: {call_improvement:+.1f}%")
    print(f"Peak memory reduction: {memory_improvement:+.1f}%")
    
    if call_improvement > 0:
        print(f"✓ ZeroCopy reduces allocation calls by {call_improvement:.1f}%")
    else:
        print(f"✗ ZeroCopy increases allocation calls by {abs(call_improvement):.1f}%")
    
    if memory_improvement > 0:
        print(f"✓ ZeroCopy reduces peak memory by {memory_improvement:.1f}%")
    else:
        print(f"✗ ZeroCopy increases peak memory by {abs(memory_improvement):.1f}%")

if __name__ == "__main__":
    main()