#!/bin/bash

# Performance measurement script for zerocopy vs regular mode
# This script measures CPU usage, memory consumption, and system calls

set -e

DURATION=${1:-120}  # Test duration in seconds (default: 2 minutes)
SNAPSERVER_PID=""
CLIENT_COUNT=${2:-2}  # Number of clients to simulate (default: 2)

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo_color() {
    echo -e "${2}${1}${NC}"
}

cleanup() {
    echo_color "Cleaning up..." $YELLOW
    if [[ -n "$SNAPSERVER_PID" ]] && kill -0 "$SNAPSERVER_PID" 2>/dev/null; then
        kill -TERM "$SNAPSERVER_PID"
        wait "$SNAPSERVER_PID" 2>/dev/null || true
    fi
    
    # Kill any running clients
    pkill -f snapclient || true
    
    # Kill monitoring processes
    pkill -f pidstat || true
    pkill -f heaptrack || true
}

trap cleanup EXIT

measure_performance() {
    local mode=$1
    local output_prefix=$2
    
    echo_color "=== Testing $mode mode ===" $GREEN
    
    # Start snapserver
    if [[ "$mode" == "zerocopy" ]]; then
        echo_color "Starting snapserver with zerocopy enabled..." $YELLOW
        ./bin/snapserver -z -c snapserver2.conf > "${output_prefix}_server.log" 2>&1 &
    else
        echo_color "Starting snapserver in regular mode..." $YELLOW  
        ./bin/snapserver -c snapserver2.conf > "${output_prefix}_server.log" 2>&1 &
    fi
    
    SNAPSERVER_PID=$!
    echo_color "Snapserver PID: $SNAPSERVER_PID" $YELLOW
    
    # Wait for server to start
    sleep 5
    
    # Start performance monitoring
    echo_color "Starting performance monitoring..." $YELLOW
    
    # CPU and memory monitoring with pidstat
    pidstat -p "$SNAPSERVER_PID" -u -r -s 1 > "${output_prefix}_pidstat.txt" &
    PIDSTAT_PID=$!
    
    # System call tracing (sample for 30 seconds to avoid overhead)
    echo_color "Starting system call tracing for 30 seconds..." $YELLOW
    timeout 30s strace -c -p "$SNAPSERVER_PID" 2> "${output_prefix}_strace.txt" || true &
    
    # Memory profiling with heaptrack (if available)
    if command -v heaptrack &> /dev/null; then
        echo_color "Starting heaptrack memory profiling..." $YELLOW
        heaptrack --pid "$SNAPSERVER_PID" -o "${output_prefix}_heaptrack" &
        HEAPTRACK_PID=$!
    fi
    
    # Start clients to generate load
    echo_color "Starting $CLIENT_COUNT clients..." $YELLOW
    for i in $(seq 1 $CLIENT_COUNT); do
        ./bin/snapclient -h localhost > "${output_prefix}_client${i}.log" 2>&1 &
        echo_color "Started client $i" $YELLOW
        sleep 2  # Stagger client connections
    done
    
    echo_color "Running test for $DURATION seconds..." $GREEN
    sleep $DURATION
    
    echo_color "Stopping clients..." $YELLOW
    pkill -f snapclient || true
    
    echo_color "Stopping server..." $YELLOW
    kill -TERM "$SNAPSERVER_PID"
    wait "$SNAPSERVER_PID" 2>/dev/null || true
    
    echo_color "Stopping monitoring..." $YELLOW
    kill -TERM "$PIDSTAT_PID" 2>/dev/null || true
    
    if [[ -n "${HEAPTRACK_PID:-}" ]]; then
        kill -TERM "$HEAPTRACK_PID" 2>/dev/null || true
    fi
    
    sleep 2
    echo_color "$mode test completed!" $GREEN
}

analyze_results() {
    echo_color "=== Performance Analysis ===" $GREEN
    
    echo_color "CPU Usage Analysis:" $YELLOW
    echo "Regular mode average CPU:"
    if [[ -f "regular_pidstat.txt" ]]; then
        grep -v "^$" regular_pidstat.txt | grep -v "Average\|Linux\|#\|PID" | awk '{sum+=$4+$5} END {if(NR>0) print "  Total CPU: " sum/NR "% (User: " sum_usr/NR "%, System: " sum_sys/NR "%)"}'
    fi
    
    echo "Zerocopy mode average CPU:"
    if [[ -f "zerocopy_pidstat.txt" ]]; then
        grep -v "^$" zerocopy_pidstat.txt | grep -v "Average\|Linux\|#\|PID" | awk '{sum+=$4+$5} END {if(NR>0) print "  Total CPU: " sum/NR "% (User: " sum_usr/NR "%, System: " sum_sys/NR "%)"}'
    fi
    
    echo_color "Memory Usage Analysis:" $YELLOW
    echo "Regular mode average memory:"
    if [[ -f "regular_pidstat.txt" ]]; then
        grep -v "^$" regular_pidstat.txt | grep -v "Average\|Linux\|#\|PID" | awk '{sum+=$6} END {if(NR>0) print "  RSS: " sum/NR " kB"}'
    fi
    
    echo "Zerocopy mode average memory:"  
    if [[ -f "zerocopy_pidstat.txt" ]]; then
        grep -v "^$" zerocopy_pidstat.txt | grep -v "Average\|Linux\|#\|PID" | awk '{sum+=$6} END {if(NR>0) print "  RSS: " sum/NR " kB"}'
    fi
    
    echo_color "System Call Analysis:" $YELLOW
    echo "Regular mode top system calls:"
    if [[ -f "regular_strace.txt" ]]; then
        echo "  Top 5 system calls:"
        tail -n 20 regular_strace.txt | head -n 10 | sort -nr -k2 | head -n 5
    fi
    
    echo "Zerocopy mode top system calls:"
    if [[ -f "zerocopy_strace.txt" ]]; then
        echo "  Top 5 system calls:"
        tail -n 20 zerocopy_strace.txt | head -n 10 | sort -nr -k2 | head -n 5
    fi
    
    echo_color "Log Analysis:" $YELLOW
    echo "Regular mode server log size:"
    if [[ -f "regular_server.log" ]]; then
        wc -l regular_server.log
    fi
    
    echo "Zerocopy mode server log size:"
    if [[ -f "zerocopy_server.log" ]]; then
        wc -l zerocopy_server.log
    fi
    
    # Check if heaptrack files exist and analyze
    if [[ -f "regular_heaptrack.gz" && -f "zerocopy_heaptrack.gz" ]]; then
        echo_color "Memory Allocation Analysis:" $YELLOW
        echo "Use: heaptrack_print regular_heaptrack.gz | head -50"
        echo "     heaptrack_print zerocopy_heaptrack.gz | head -50"
    fi
}

# Main execution
cd "$(dirname "$0")/.."

echo_color "Snapcast Zerocopy Performance Measurement" $GREEN
echo_color "Test duration: $DURATION seconds" $YELLOW
echo_color "Number of clients: $CLIENT_COUNT" $YELLOW
echo ""

# Test regular mode
measure_performance "regular" "regular"

echo_color "Waiting 10 seconds between tests..." $YELLOW
sleep 10

# Test zerocopy mode  
measure_performance "zerocopy" "zerocopy"

# Analyze results
analyze_results

echo_color "Performance measurement complete!" $GREEN
echo_color "Result files:" $YELLOW
echo "  - regular_* (regular mode results)"
echo "  - zerocopy_* (zerocopy mode results)"
echo ""
echo_color "To visualize pidstat data:" $YELLOW
echo "  pidstat data can be converted to JSON and plotted with pidplot.py"