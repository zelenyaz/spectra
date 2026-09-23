#!/usr/bin/env python3
"""
Parse /proc/pid/numa_maps file and calculate statistics.

This script reads numa_maps file and calculates the total sum of:
mapped, anon, dirty, active, N0, N1 in pages and converts to GB.

Supports monitoring mode to periodically check NUMA node memory usage.
"""

import os
import re
import sys
import time
import argparse
from datetime import datetime
from typing import Dict, List, Optional


def get_tids(pid: int) -> List[int]:
    """Return all TIDs belonging to a PID by reading /proc/<pid>/task/."""
    task_dir = f"/proc/{pid}/task"
    try:
        tids = sorted(int(d) for d in os.listdir(task_dir) if d.isdigit())
    except (FileNotFoundError, PermissionError):
        return []
    return tids


def print_tids(pid: int):
    """Print all TIDs belonging to a PID."""
    tids = get_tids(pid)
    if not tids:
        print(f"No threads found for PID {pid}")
        return
    print(f"PID {pid}: {len(tids)} thread(s)")
    print(f"  TIDs: {', '.join(str(t) for t in tids)}")
    print()


def extract_pid_from_path(path: str) -> Optional[int]:
    """Extract PID from a /proc/<pid>/numa_maps path."""
    m = re.match(r'/proc/(\d+)/', path)
    if m:
        return int(m.group(1))
    return None


def parse_numa_maps(file_path: str) -> Dict[str, int]:
    """
    Parse numa_maps file and extract statistics.
    
    Args:
        file_path: Path to the numa_maps file
        
    Returns:
        Dictionary with summed statistics
    """
    stats = {
        'mapped': 0,
        'anon': 0,
        'dirty': 0,
        'active': 0,
        'N0': 0,
        'N1': 0,
        'total_pages': 0
    }
    
    # Pattern to match key=value pairs
    pattern = re.compile(r'(\w+)=(\d+)')
    
    with open(file_path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            
            # Find all key=value pairs in the line
            matches = pattern.findall(line)
            
            for key, value in matches:
                if key in stats:
                    stats[key] += int(value)
    
    return stats


def pages_to_gb(pages: int, page_size_kb: int = 4) -> float:
    """
    Convert pages to GB.
    
    Args:
        pages: Number of pages
        page_size_kb: Page size in KB (default 4KB)
        
    Returns:
        Size in GB
    """
    kb = pages * page_size_kb
    gb = kb / (1024 * 1024)
    return gb


def print_full_statistics(stats: Dict[str, int]):
    """Print full statistics table."""
    print("=" * 60)
    print("NUMA Maps Statistics")
    print("=" * 60)
    print()
    
    # Print statistics in pages and GB
    print(f"{'Metric':<15} {'Pages':>15} {'GB':>15}")
    print("-" * 60)
    
    for key in ['mapped', 'anon', 'dirty', 'active', 'N0', 'N1']:
        pages = stats[key]
        gb = pages_to_gb(pages)
        print(f"{key:<15} {pages:>15,} {gb:>15.4f}")
    
    print("-" * 60)
    
    # Calculate totals
    total_pages = sum(stats[key] for key in ['mapped', 'anon', 'dirty', 'active', 'N0', 'N1'])
    total_gb = pages_to_gb(total_pages)
    
    print(f"{'Total':<15} {total_pages:>15,} {total_gb:>15.4f}")
    print()
    
    # Additional useful metrics
    print("=" * 60)
    print("Additional Metrics")
    print("=" * 60)
    print()
    
    # N0 + N1 (total memory across NUMA nodes)
    numa_total_pages = stats['N0'] + stats['N1']
    numa_total_gb = pages_to_gb(numa_total_pages)
    print(f"NUMA Total (N0+N1): {numa_total_pages:,} pages = {numa_total_gb:.4f} GB")
    
    # Memory distribution
    if numa_total_pages > 0:
        n0_percent = (stats['N0'] / numa_total_pages) * 100
        n1_percent = (stats['N1'] / numa_total_pages) * 100
        print(f"N0 percentage: {n0_percent:.2f}%")
        print(f"N1 percentage: {n1_percent:.2f}%")
    
    # Dirty vs Clean (mapped - dirty = clean pages)
    if stats['mapped'] > 0 and stats['dirty'] > 0:
        clean_pages = stats['mapped'] - stats['dirty']
        clean_gb = pages_to_gb(clean_pages) if clean_pages > 0 else 0
        print(f"Clean pages: {clean_pages:,} pages = {clean_gb:.4f} GB")
    
    print()


def monitor_numa_memory(pid: int, interval: int = 1):
    """
    Monitor NUMA memory usage for a given PID at regular intervals.
    
    Args:
        pid: Process ID to monitor
        interval: Sampling interval in seconds
    """
    numa_maps_path = f"/proc/{pid}/numa_maps"
    
    print("=" * 70, flush=True)
    print(f"Monitoring NUMA memory for PID {pid} (interval: {interval}s)", flush=True)
    print("Press Ctrl+C to stop", flush=True)
    print("=" * 70, flush=True)
    print(flush=True)
    print_tids(pid)
    print(f"{'Timestamp':<20} {'N0 (GB)':>15} {'N1 (GB)':>15} {'Total (GB)':>15}", flush=True)
    print("-" * 70, flush=True)
    
    try:
        while True:
            try:
                stats = parse_numa_maps(numa_maps_path)
                
                n0_gb = pages_to_gb(stats['N0'])
                n1_gb = pages_to_gb(stats['N1'])
                total_gb = n0_gb + n1_gb
                
                timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                print(f"{timestamp:<20} {n0_gb:>15.4f} {n1_gb:>15.4f} {total_gb:>15.4f}", flush=True)
                
                time.sleep(interval)
                
            except FileNotFoundError:
                print(f"Error: Process {pid} not found or terminated.", flush=True)
                break
            except PermissionError:
                print(f"Error: Permission denied to read /proc/{pid}/numa_maps", flush=True)
                break
                
    except KeyboardInterrupt:
        print(flush=True)
        print("Monitoring stopped.", flush=True)
        print(flush=True)


def main():
    """Main function."""
    parser = argparse.ArgumentParser(
        description='Parse and monitor NUMA maps statistics',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Parse a numa_maps file
  %(prog)s /proc/1234/numa_maps
  %(prog)s bench/llama.numa.maps
  
  # Monitor a running process every 1 second
  %(prog)s --monitor --pid 1234
  %(prog)s -m -p 1234
  
  # Monitor with custom interval (5 seconds)
  %(prog)s --monitor --pid 1234 --interval 5
  %(prog)s -m -p 1234 -i 5
        """
    )
    
    parser.add_argument(
        'file',
        nargs='?',
        help='Path to numa_maps file (e.g., /proc/1234/numa_maps)'
    )
    parser.add_argument(
        '-m', '--monitor',
        action='store_true',
        help='Enable monitoring mode'
    )
    parser.add_argument(
        '-p', '--pid',
        type=int,
        help='Process ID to monitor (requires --monitor)'
    )
    parser.add_argument(
        '-i', '--interval',
        type=int,
        default=1,
        help='Sampling interval in seconds (default: 1)'
    )
    
    args = parser.parse_args()
    
    # Monitor mode
    if args.monitor:
        if not args.pid:
            print("Error: --pid is required in monitor mode")
            parser.print_help()
            sys.exit(1)
        
        monitor_numa_memory(args.pid, args.interval)
        return
    
    # File parsing mode
    if not args.file:
        print("Error: file path is required in non-monitor mode")
        parser.print_help()
        sys.exit(1)
    
    file_path = args.file
    
    try:
        pid = extract_pid_from_path(file_path)
        if pid:
            print_tids(pid)

        stats = parse_numa_maps(file_path)
        print_full_statistics(stats)
        
    except FileNotFoundError:
        print(f"Error: File '{file_path}' not found.")
        sys.exit(1)
    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
