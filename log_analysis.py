#!/usr/bin/env python3
import os
import argparse
from pathlib import Path
import glob
import re
import pandas as pd
import matplotlib.pyplot as plt
from datetime import datetime, timedelta
from collections import defaultdict

plt.rcParams['axes.grid'] = True

def extract_start_time(log_path):
    """
    从日志文件名中提取开始时间
    """
    match = re.search(r'_(\d{8})_(\d{6})\.log$', log_path)
    if match:
        date_str = match.group(1)  # '20241217'
        time_str = match.group(2)  # '222151'
        try:
            start_time = datetime.strptime(f"{date_str} {time_str}", "%Y%m%d %H%M%S")
            return start_time
        except ValueError as ve:
            print(f"Error parsing start date: {ve}")
            return datetime.now()
    else:
        print("Cannot extract date from filename, use current date.")
        return datetime.now()

def get_latest_timestamp(log_dir):
    """
    获取logs目录中最新的时间戳
    """
    log_files = glob.glob(os.path.join(log_dir, "*.log"))
    timestamp_dict = defaultdict(set)
    for file in log_files:
        filename = os.path.basename(file)
        match = re.match(r'(\w+)_(\d{8}_\d{6})\.log$', filename)
        if match:
            log_type = match.group(1)
            timestamp = match.group(2)
            timestamp_dict[log_type].add(timestamp)
    
    # 找到所有日志类型共有的时间戳
    if not timestamp_dict:
        return None
    common_timestamps = set.intersection(*timestamp_dict.values())
    if not common_timestamps:
        print("No common timestamp found across log files.")
        return None
    
    # 返回最新的时间戳
    latest_timestamp = max(common_timestamps)
    return latest_timestamp

def get_log_file(log_dir, log_type, timestamp):
    """
    根据日志类型和时间戳获取日志文件路径
    """
    return os.path.join(log_dir, f"{log_type}_{timestamp}.log")

def get_log_files(log_dir):
    """
    自动获取所有日志文件的路径，基于最新的共同时间戳
    """
    latest_timestamp = get_latest_timestamp(log_dir)
    if not latest_timestamp:
        print("Failed to find a common timestamp among log files.")
        return {}
    
    print(f"Using timestamp: {latest_timestamp}")
    
    log_types = ['vmstat', 'iostat', 'disk_usage', 'pidstat', 'stress_test']
    log_files = {}
    for log_type in log_types:
        log_file = get_log_file(log_dir, log_type, latest_timestamp)
        if os.path.exists(log_file):
            log_files[log_type] = log_file
    
    return log_files

def parse_vmstat(log_path, sampling_interval=60, start_time=None):
    """
    解析vmstat日志文件
    """
    with open(log_path, 'r') as f:
        lines = f.readlines()

    # 找到数据行的起始点，跳过标题
    data_lines = []
    headers = []
    for i, line in enumerate(lines):
        if line.startswith('procs'):
            # 下一行是标题行
            headers = lines[i + 1].strip().split()
            skip_lines = i + 2  # 数据从这里开始
            break
    else:
        print("未找到 'procs' 行，无法解析 vmstat 日志。")
        return pd.DataFrame()

    # 读取数据行
    for line in lines[skip_lines:]:
        if re.match(r'\s*\d+', line):
            data = line.strip().split()
            data_lines.append(data)

    # 创建DataFrame
    vmstat_df = pd.DataFrame(data_lines, columns=headers)
    # 转换数据类型
    for col in vmstat_df.columns:
        vmstat_df[col] = pd.to_numeric(vmstat_df[col], errors='coerce')

    # 添加时间戳
    if start_time:
        vmstat_df['Timestamp'] = pd.date_range(start=start_time, periods=len(vmstat_df), freq=f'{sampling_interval}s')
    else:
        vmstat_df['Timestamp'] = pd.date_range(start=datetime.now(), periods=len(vmstat_df), freq=f'{sampling_interval}s')
    vmstat_df.set_index('Timestamp', inplace=True)

    return vmstat_df

def parse_iostat(log_path, sampling_interval=60, start_time=None):
    """
    解析iostat日志文件
    """
    with open(log_path, 'r') as f:
        lines = f.readlines()

    cpu_data = []
    device_data = []
    current_section = None
    headers_cpu = []
    headers_device = []
    sample_index = -1
    for line in lines:
        line = line.strip()
        if line.startswith("avg-cpu"):
            sample_index += 1
            current_section = 'cpu'
            headers_cpu = []
            continue
        elif line.startswith("Device"):
            current_section = 'device'
            headers_device = line.split()
            continue
        elif not line or line.startswith("Linux"):
            continue
        else:
            if current_section == 'cpu':
                data = line.split()
                if not headers_cpu:
                    headers_cpu = ['%user', '%nice', '%system', '%iowait', '%steal', '%idle']
                if len(data) == len(headers_cpu):
                    cpu_entry = dict(zip(headers_cpu, data))
                    cpu_data.append(cpu_entry)
            elif current_section == 'device':
                data = line.split()
                if len(data) >= len(headers_device):
                    device_entry = dict(zip(headers_device, data))
                    device_entry['sample'] = max(0, sample_index)
                    device_data.append(device_entry)

    # 创建CPU DataFrame
    iostat_cpu_df = pd.DataFrame(cpu_data)
    for col in iostat_cpu_df.columns:
        iostat_cpu_df[col] = pd.to_numeric(iostat_cpu_df[col], errors='coerce')
    if start_time:
        iostat_cpu_df['Timestamp'] = pd.date_range(start=start_time, periods=len(iostat_cpu_df), freq=f'{sampling_interval}s')
    else:
        iostat_cpu_df['Timestamp'] = pd.date_range(start=datetime.now(), periods=len(iostat_cpu_df), freq=f'{sampling_interval}s')
    iostat_cpu_df.set_index('Timestamp', inplace=True)

    # 创建Device DataFrame
    iostat_device_df = pd.DataFrame(device_data)
    for col in iostat_device_df.columns:
        if col not in ['Device']:
            iostat_device_df[col] = pd.to_numeric(iostat_device_df[col], errors='coerce')
    if not iostat_device_df.empty:
        timestamps = pd.to_timedelta(iostat_device_df.pop('sample') * sampling_interval, unit='s')
        iostat_device_df['Timestamp'] = (start_time or datetime.now()) + timestamps
        iostat_device_df.set_index('Timestamp', inplace=True)
    return iostat_cpu_df, iostat_device_df

def disk_gib(text):
    match = re.fullmatch(r'(\d+(?:\.\d+)?)([KMGTPE]?)(?:i?B?)', text, re.I)
    if not match:
        raise ValueError(f'unsupported disk size {text}')
    exponent = ' KMGTPE'.index(match[2].upper() or ' ')
    return float(match[1]) * 1024 ** (exponent - 3)

def parse_disk_usage(log_path, sampling_interval=300, start_time=None):
    """
    解析disk_usage日志文件
    """
    with open(log_path, 'r') as f:
        lines = f.readlines()

    disk_data = []
    headers = []
    for line in lines:
        if line.startswith("Filesystem"):
            headers = line.strip().split()
            continue
        if line.strip() and not line.startswith('Filesystem'):
            data = line.strip().split()
            if len(data) < 5:
                continue
            try:
                size_str = data[1]
                used_str = data[2]
                avail_str = data[3]
                use_percent_str = data[4]

                size, used, avail = (disk_gib(x) for x in (size_str, used_str, avail_str))
                use_percent = float(use_percent_str.replace('%', ''))

                disk_entry = {
                    'Filesystem': data[0],
                    'Size': size,
                    'Used': used,
                    'Avail': avail,
                    'Use%': use_percent
                }
                disk_data.append(disk_entry)
            except ValueError as ve:
                print(f"Error parsing disk usage line: {ve}")
                continue

    # 创建DataFrame
    disk_df = pd.DataFrame(disk_data)
    if disk_df.empty:
        print("disk_df is empty. Please check the disk_usage log file format.")
    else:
        # 添加时间戳
        if start_time:
            disk_df['Timestamp'] = pd.date_range(start=start_time, periods=len(disk_df), freq=f'{sampling_interval}s')
        else:
            disk_df['Timestamp'] = pd.date_range(start=datetime.now(), periods=len(disk_df), freq=f'{sampling_interval}s')
        disk_df.set_index('Timestamp', inplace=True)

        # print(f"Disk Usage DataFrame head:\n{disk_df.head()}")  
        # print(f"Disk Usage DataFrame start time: {disk_df.index[0]}")  
        # print(f"Disk Usage DataFrame end time: {disk_df.index[-1]}")  

    return disk_df

def parse_pidstat(log_path, start_time=None):
    """Read CPU and memory samples using the column names of each pidstat section."""
    day = (start_time or extract_start_time(log_path)).date()
    previous_time = None
    header = []
    cpu_rows, memory_rows = [], []
    with open(log_path) as stream:
        for line in stream:
            parts = line.split()
            if not parts or not re.fullmatch(r'\d{2}:\d{2}:\d{2}', parts[0]):
                continue
            clock_text = parts.pop(0)
            clock_format = '%H:%M:%S'
            if parts and parts[0] in ('AM', 'PM'):
                clock_text += ' ' + parts.pop(0)
                clock_format = '%I:%M:%S %p'
            if 'PID' in parts and 'Command' in parts:
                header = parts
                continue
            if not header or len(parts) < len(header):
                continue

            fields = dict(zip(header, parts))
            if '%CPU' in fields:
                samples = cpu_rows
            elif '%MEM' in fields:
                samples = memory_rows
            else:
                continue
            sample_time = datetime.strptime(clock_text, clock_format).time()
            if previous_time is not None and sample_time < previous_time:
                day += timedelta(days=1)
            previous_time = sample_time
            fields['Timestamp'] = datetime.combine(day, sample_time)
            fields['Command'] = ' '.join(parts[len(header) - 1:])
            samples.append(fields)

    def frame(rows):
        data = pd.DataFrame(rows)
        if data.empty:
            return data
        data.set_index('Timestamp', inplace=True)
        for column in data.columns.difference(['Command', 'UID']):
            data[column] = pd.to_numeric(data[column], errors='coerce')
        return data

    return frame(cpu_rows), frame(memory_rows)

def parse_stress_test_log(log_path):
    """
    解析stress_test日志文件
    """
    records = []
    old = re.compile(r'\[Stats\] Elapsed: (\d+)s .* Throughput: ([\d.]+) ops/s')
    new = re.compile(r'\[Window\] elapsed_s=([\d.]+) window_s=[\d.]+ operations=\d+ ops_per_sec=([\d.]+)')
    with open(log_path, 'r') as stream:
        for line in stream:
            match = new.search(line) or old.search(line)
            if match:
                records.append((float(match[1]), float(match[2])))
    return pd.DataFrame(records, columns=['Elapsed Time (s)', 'Throughput (ops/s)'])

def plot_cpu_usage(vmstat_df, iostat_cpu_df, pidstat_cpu_df, charts_dir):
    """
    绘制CPU使用率图表
    """
    plt.figure(figsize=(14, 7))

    # vmstat CPU
    plt.plot(vmstat_df.index, vmstat_df['us'], label='vmstat %user', color='blue')
    plt.plot(vmstat_df.index, vmstat_df['sy'], label='vmstat %system', color='orange')
    plt.plot(vmstat_df.index, vmstat_df['id'], label='vmstat %idle', color='green')
    plt.plot(vmstat_df.index, vmstat_df['wa'], label='vmstat %iowait', color='red')

    # iostat CPU
    plt.plot(iostat_cpu_df.index, iostat_cpu_df['%user'], label='iostat %user', linestyle='--', color='blue')
    plt.plot(iostat_cpu_df.index, iostat_cpu_df['%system'], label='iostat %system', linestyle='--', color='orange')
    plt.plot(iostat_cpu_df.index, iostat_cpu_df['%idle'], label='iostat %idle', linestyle='--', color='green')
    plt.plot(iostat_cpu_df.index, iostat_cpu_df['%iowait'], label='iostat %iowait', linestyle='--', color='red')

    # pidstat CPU
    if not pidstat_cpu_df.empty:
        plt.plot(pidstat_cpu_df.index, pidstat_cpu_df['%CPU'], label='pidstat %CPU', marker='o', color='purple')

    plt.xlabel('Time')
    plt.ylabel('CPU Usage (%)')
    plt.title('CPU Usage Over Time')
    plt.legend()
    plt.tight_layout()
    plt.savefig(charts_dir / 'cpu_usage.png')
    plt.close()

def plot_memory_usage(vmstat_df, pidstat_mem_df, charts_dir):
    """
    绘制内存使用率图表，使用双 Y 轴
    """
    plt.figure(figsize=(14, 7))
    
    # 创建主轴
    ax1 = plt.gca()
    
    # 绘制vmstat内存指标（KB）
    ax1.plot(vmstat_df.index, vmstat_df['free'], label='vmstat Free Memory (KB)', color='blue')
    ax1.plot(vmstat_df.index, vmstat_df['buff'], label='vmstat Buffers (KB)', color='orange')
    ax1.plot(vmstat_df.index, vmstat_df['cache'], label='vmstat Cache (KB)', color='green')
    
    ax1.set_xlabel('Time')
    ax1.set_ylabel('Memory Usage (KB)')
    ax1.tick_params(axis='y')
    
    # 创建第二个Y轴
    ax2 = ax1.twinx()
    
    # 绘制pidstat %MEM（%）
    if not pidstat_mem_df.empty:
        ax2.plot(pidstat_mem_df.index, pidstat_mem_df['%MEM'], label='pidstat %MEM', marker='o', color='purple')
    
    ax2.set_ylabel('Memory Usage (%MEM)')
    ax2.tick_params(axis='y')
    
    # 合并图例
    lines_1, labels_1 = ax1.get_legend_handles_labels()
    lines_2, labels_2 = ax2.get_legend_handles_labels()
    ax1.legend(lines_1 + lines_2, labels_1 + labels_2, loc='upper left')
    
    plt.title('Memory Usage Over Time')
    plt.tight_layout()
    plt.savefig(charts_dir / 'memory_usage.png')
    plt.close()

def plot_disk_io(iostat_device_df, charts_dir):
    """
    绘制磁盘IO图表
    """
    required = {'Device', 'rkB/s', 'wkB/s', '%util'}
    if not required.issubset(iostat_device_df.columns):
        return
    fig, (rates, util) = plt.subplots(2, 1, figsize=(14, 8), sharex=True)
    for device, samples in iostat_device_df.groupby('Device'):
        rates.plot(samples.index, samples['rkB/s'], label=f'{device} read')
        rates.plot(samples.index, samples['wkB/s'], label=f'{device} write')
        util.plot(samples.index, samples['%util'], label=device)
    rates.set_ylabel('kB/s')
    util.set_ylabel('Utilization (%)')
    util.set_xlabel('Time')
    rates.legend()
    util.legend()
    fig.tight_layout()
    fig.savefig(charts_dir / 'disk_io.png')
    plt.close(fig)

def plot_disk_usage(disk_df, charts_dir):
    """
    绘制磁盘使用情况图表
    """
    # print(f"Plotting Disk Usage from {disk_df.index.min()} to {disk_df.index.max()}")  
    
    plt.figure(figsize=(14, 7))

    plt.plot(disk_df.index, disk_df['Used'], label='Used (GiB)', color='blue')
    plt.plot(disk_df.index, disk_df['Avail'], label='Available (GiB)', color='orange')
    plt.plot(disk_df.index, disk_df['Use%'], label='Use% (%)', color='green')

    plt.xlabel('Time')
    plt.ylabel('Disk Usage (GiB) / Usage (%)')
    plt.title('Disk Usage Over Time')
    plt.legend()
    plt.tight_layout()
    plt.savefig(charts_dir / 'disk_usage.png')
    plt.close()

def plot_process_metrics(pidstat_cpu_df, pidstat_mem_df, charts_dir):
    """
    绘制进程的CPU和内存使用情况图表
    """
    plt.figure(figsize=(14, 7))

    if not pidstat_cpu_df.empty:
        plt.plot(pidstat_cpu_df.index, pidstat_cpu_df['%CPU'], label='Process %CPU', color='blue')
    if not pidstat_mem_df.empty:
        plt.plot(pidstat_mem_df.index, pidstat_mem_df['%MEM'], label='Process %MEM', color='orange')

    plt.xlabel('Time')
    plt.ylabel('CPU Usage (%) / Memory Usage (%)')
    plt.title('Process CPU and Memory Usage Over Time')
    plt.legend()
    plt.tight_layout()
    plt.savefig(charts_dir / 'process_metrics.png')
    plt.close()

def plot_stress_test_throughput(df, charts_dir):
    """
    绘制stress_test的吞吐量图表
    """
    plt.figure(figsize=(14, 7))
    plt.plot(df['Elapsed Time (s)'], df['Throughput (ops/s)'], label='Throughput (ops/s)', color='blue')
    plt.xlabel('Elapsed Time (s)')
    plt.ylabel('Throughput (ops/s)')
    plt.title('Stress Test Throughput Over Time')
    plt.legend()
    plt.tight_layout()
    plt.savefig(charts_dir / 'stress_test_throughput.png')
    plt.close()

def main():
    parser = argparse.ArgumentParser(description='Plot one KV stress run and available Linux resource samples.')
    parser.add_argument('--log-dir', help='Directory printed by run_stress_test.sh')
    parser.add_argument('--charts-dir', help='Output directory (default: charts/ within the selected log directory)')
    args = parser.parse_args()
    if args.log_dir:
        log_dir = Path(args.log_dir)
    else:
        runs = sorted(path for path in Path('logs').glob('run_*') if path.is_dir())
        log_dir = runs[-1] if runs else Path('logs')
    log_files = get_log_files(str(log_dir))
    if not log_files or 'stress_test' not in log_files:
        parser.exit(1, f'No stress log in {log_dir}\n')
    charts_dir = Path(args.charts_dir) if args.charts_dir else log_dir / 'charts'
    charts_dir.mkdir(parents=True, exist_ok=True)
    start_time = extract_start_time(log_files['stress_test'])
    vmstat_df = pd.DataFrame()
    iostat_cpu_df, iostat_device_df = pd.DataFrame(), pd.DataFrame()
    pidstat_cpu_df, pidstat_mem_df = pd.DataFrame(), pd.DataFrame()
    disk_df = pd.DataFrame()
    if 'vmstat' in log_files:
        vmstat_df = parse_vmstat(log_files['vmstat'], start_time=start_time)
    if 'iostat' in log_files:
        iostat_cpu_df, iostat_device_df = parse_iostat(log_files['iostat'], start_time=start_time)
    if 'disk_usage' in log_files:
        disk_df = parse_disk_usage(log_files['disk_usage'], start_time=start_time)
    if 'pidstat' in log_files:
        pidstat_cpu_df, pidstat_mem_df = parse_pidstat(log_files['pidstat'], start_time=start_time)
    stress_test_df = parse_stress_test_log(log_files['stress_test'])

    if stress_test_df.empty:
        print("No throughput samples found in the selected stress log.")

    # 生成图表
    if not vmstat_df.empty and not iostat_cpu_df.empty and not pidstat_cpu_df.empty:
        print("Generating CPU usage chart...")
        plot_cpu_usage(vmstat_df, iostat_cpu_df, pidstat_cpu_df, charts_dir)

    if not vmstat_df.empty and not pidstat_mem_df.empty:
        print("Generating memory usage chart...")
        plot_memory_usage(vmstat_df, pidstat_mem_df, charts_dir)

    if not iostat_device_df.empty:
        print("Generating Disk IO chart...")
        plot_disk_io(iostat_device_df, charts_dir)

    if not disk_df.empty:
        print("Generating Disk Usage chart...")
        plot_disk_usage(disk_df, charts_dir)

    if not pidstat_cpu_df.empty or not pidstat_mem_df.empty:
        print("Generating Process CPU and Memory Usage chart...")
        plot_process_metrics(pidstat_cpu_df, pidstat_mem_df, charts_dir)
        
    if not stress_test_df.empty:
        plot_stress_test_throughput(stress_test_df, charts_dir)

    print(f"Charts directory: {charts_dir}")

if __name__ == "__main__":
    main()
