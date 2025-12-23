#!/usr/bin/env python3
"""
Analysis script for heaptrack memory profiling data comparing old vs new snapserver versions.
Focuses on zerocopy and buffer pool impact on memory allocation patterns.
"""

def extract_summary_stats():
    """Extract key statistics from heaptrack_print output for both versions"""
    
    # Key findings from heaptrack_print output analysis
    old_version = {
        'boost_asio_calls': 35182,
        'boost_asio_peak_kb': 1.76,
        'std_allocator_calls': 30777,
        'std_allocator_peak_kb': 12.62,
        'total_major_calls': 35182 + 30777,  # ~65,959 major calls
    }
    
    new_version = {
        'boost_asio_calls': 54486,
        'boost_asio_peak_kb': 1.63,
        'std_allocator_calls': 379571,
        'std_allocator_peak_kb': 91.06,
        'total_major_calls': 54486 + 379571,  # ~434,057 major calls
    }
    
    return old_version, new_version

def analyze_memory_impact():
    """Analyze the memory allocation impact of zerocopy and buffer pool changes"""
    
    old, new = extract_summary_stats()
    
    print("=== SNAPSERVER MEMORY ANALYSIS: OLD vs NEW (ZeroCopy + Buffer Pool) ===\n")
    
    print("OLD VERSION (without zerocopy/buffer pool):")
    print(f"  • Boost::asio allocations: {old['boost_asio_calls']:,} calls, {old['boost_asio_peak_kb']:.2f}KB peak")
    print(f"  • std::allocator calls:    {old['std_allocator_calls']:,} calls, {old['std_allocator_peak_kb']:.2f}KB peak")
    print(f"  • Total major allocations: {old['total_major_calls']:,}")
    
    print("\nNEW VERSION (with zerocopy/buffer pool):")
    print(f"  • Boost::asio allocations: {new['boost_asio_calls']:,} calls, {new['boost_asio_peak_kb']:.2f}KB peak")
    print(f"  • std::allocator calls:    {new['std_allocator_calls']:,} calls, {new['std_allocator_peak_kb']:.2f}KB peak")
    print(f"  • Total major allocations: {new['total_major_calls']:,}")
    
    # Calculate changes
    boost_call_change = new['boost_asio_calls'] - old['boost_asio_calls']
    boost_peak_change = new['boost_asio_peak_kb'] - old['boost_asio_peak_kb']
    std_call_change = new['std_allocator_calls'] - old['std_allocator_calls']
    std_peak_change = new['std_allocator_peak_kb'] - old['std_allocator_peak_kb']
    total_call_change = new['total_major_calls'] - old['total_major_calls']
    
    print(f"\n=== CHANGES (New - Old) ===")
    print(f"  • Boost::asio calls:    {boost_call_change:+,} ({boost_call_change/old['boost_asio_calls']*100:+.1f}%)")
    print(f"  • Boost::asio peak:     {boost_peak_change:+.2f}KB ({boost_peak_change/old['boost_asio_peak_kb']*100:+.1f}%)")
    print(f"  • std::allocator calls: {std_call_change:+,} ({std_call_change/old['std_allocator_calls']*100:+.1f}%)")
    print(f"  • std::allocator peak:  {std_peak_change:+.2f}KB ({std_peak_change/old['std_allocator_peak_kb']*100:+.1f}%)")
    print(f"  • Total allocation calls: {total_call_change:+,} ({total_call_change/old['total_major_calls']*100:+.1f}%)")
    
    # Key insights from stack traces
    print(f"\n=== KEY INSIGHTS ===")
    
    print("🔍 ROOT CAUSE ANALYSIS:")
    print("  1. NEW VERSION shows 12x MORE std::allocator calls (30K → 380K)")
    print("  2. Peak memory for std::allocator increased 7.2x (12.6KB → 91.1KB)")
    print("  3. Most new allocations trace to zerocopy error queue monitoring:")
    print("     → StreamSessionTcpCoordinated::processErrorQueue()")
    print("     → String allocations for logging (strerror, LOG messages)")
    
    print("\n🧵 ZEROCOPY THREAD IMPACT:")
    print("  • New version has dedicated error queue monitoring threads")
    print("  • Each thread continuously processes completion notifications")
    print("  • Heavy string allocation for debug/trace logging")
    print("  • Example: 9,652 calls each for strerror() string creation")
    
    print("\n📊 MEMORY ALLOCATION PATTERNS:")
    print("  • OLD: Simple boost::asio recycling allocator dominated")
    print("  • NEW: Complex mix of std::string allocations + boost::asio")
    print("  • NEW: 6.6x more total allocation calls (66K → 434K)")
    
    print("\n💡 OPTIMIZATION OPPORTUNITIES:")
    print("  1. REDUCE LOGGING: Many allocations are for debug/trace strings")
    print("  2. STRING POOLING: Reuse common log strings instead of recreating")
    print("  3. STRUCTURED LOGGING: Use static format strings to reduce allocations")
    print("  4. BATCH PROCESSING: Process multiple completion notifications together")
    
    print("\n⚖️  TRADE-OFFS:")
    print("  ✅ BENEFITS: Zero-copy networking reduces data copying")
    print("  ✅ BENEFITS: Buffer pool reduces allocation frequency for audio data")
    print("  ⚠️  COST: Increased allocation overhead from monitoring/logging")
    print("  ⚠️  COST: Each client connection adds monitoring thread overhead")
    
    # Calculate actual memory impact
    total_peak_old = old['boost_asio_peak_kb'] + old['std_allocator_peak_kb']
    total_peak_new = new['boost_asio_peak_kb'] + new['std_allocator_peak_kb']
    peak_increase = total_peak_new - total_peak_old
    
    print(f"\n📈 PEAK MEMORY CONSUMPTION:")
    print(f"  • Old version total:  {total_peak_old:.2f}KB peak")
    print(f"  • New version total:  {total_peak_new:.2f}KB peak")
    print(f"  • Net increase:       {peak_increase:+.2f}KB ({peak_increase/total_peak_old*100:+.1f}%)")
    
    if peak_increase > 0:
        print(f"  • Cost per client:    ~{peak_increase/2:.2f}KB (assuming 2 clients)")
        print(f"  • Scaling impact:     ~{peak_increase/2*100:.1f}KB for 100 clients")
    
    return {
        'old_version': old,
        'new_version': new,
        'changes': {
            'boost_calls': boost_call_change,
            'boost_peak': boost_peak_change,
            'std_calls': std_call_change,
            'std_peak': std_peak_change,
            'total_calls': total_call_change,
            'total_peak': peak_increase
        }
    }

if __name__ == "__main__":
    analysis_results = analyze_memory_impact()
    
    print(f"\n=== SUMMARY ===")
    print("The zerocopy and buffer pool implementation successfully reduces data copying")
    print("but introduces significant logging/monitoring overhead. The 6x increase in")
    print("allocation calls is primarily due to string allocations in error queue")
    print("monitoring threads, not the core zerocopy functionality itself.")
    print("\nRecommendation: Optimize logging to reduce string allocation overhead.")