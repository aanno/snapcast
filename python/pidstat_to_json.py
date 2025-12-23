#!/usr/bin/env python3

"""
Convert pidstat text output to JSON format compatible with pidplot.py
"""

import sys
import json
import re
from datetime import datetime

def parse_pidstat_line(line):
    """Parse a single pidstat line and extract metrics"""
    # Example line: 10:23:45      1234   user  5.50  2.30  7.80  123456  ...
    parts = line.split()
    if len(parts) < 8:
        return None
        
    try:
        time_str = parts[0]
        pid = int(parts[1])
        user = parts[2]
        usr_pct = float(parts[3])
        system_pct = float(parts[4]) 
        total_cpu = usr_pct + system_pct
        mem_kb = float(parts[6]) if len(parts) > 6 else 0
        mem_pct = float(parts[7]) if len(parts) > 7 else 0
        
        return {
            'timestamp': time_str,
            'pid': pid,
            'user': user,
            'usr_pct': usr_pct,
            'system_pct': system_pct,
            'total_cpu': total_cpu,
            'mem_kb': mem_kb,
            'mem_pct': mem_pct
        }
    except (ValueError, IndexError):
        return None

def convert_pidstat_to_json(input_file, output_file):
    """Convert pidstat text output to JSON format for pidplot.py"""
    
    records = []
    
    with open(input_file, 'r') as f:
        for line in f:
            line = line.strip()
            
            # Skip header lines and empty lines
            if not line or line.startswith('#') or 'Linux' in line or 'Average' in line or 'PID' in line:
                continue
                
            parsed = parse_pidstat_line(line)
            if parsed:
                # Convert to pidplot.py expected format
                record = {
                    'timestamp': parsed['timestamp'],
                    'task-cpu-load': [{
                        'usr': parsed['usr_pct'],
                        'system': parsed['system_pct']
                    }],
                    'task-memory': [{
                        'MEM': parsed['mem_pct']
                    }],
                    'stack': [{
                        'StkSize': parsed['mem_kb'] / 1024  # Convert KB to MB
                    }],
                    'io': [{
                        'kB_rd/s': 0,  # pidstat -u -r doesn't include I/O
                        'kB_wr/s': 0
                    }]
                }
                records.append(record)
    
    # Create output structure compatible with pidplot.py
    output_data = {
        'sysstat': {
            'hosts': [{
                'statistics': records
            }]
        }
    }
    
    with open(output_file, 'w') as f:
        json.dump(output_data, f, indent=2)
    
    print(f"Converted {len(records)} records from {input_file} to {output_file}")

def main():
    if len(sys.argv) != 3:
        print("Usage: pidstat_to_json.py <input_pidstat.txt> <output.json>")
        print("Example: pidstat_to_json.py regular_pidstat.txt regular_pidstat.json")
        sys.exit(1)
    
    input_file = sys.argv[1]
    output_file = sys.argv[2]
    
    try:
        convert_pidstat_to_json(input_file, output_file)
        print(f"Success! You can now run: python3 pidplot.py {output_file}")
    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()