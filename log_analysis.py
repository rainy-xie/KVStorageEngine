#!/usr/bin/env python3
import os
import glob
import re
import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
from datetime import datetime, timedelta
import matplotlib.dates as mdates
from collections import defaultdict

# 设置Seaborn风格
sns.set_theme(style="whitegrid")

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
    common_timestamps = set.intersection(*[s for s in timestamp_dict.values() if s])
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
        else:
            print(f"Warning: Expected log file {log_file} does not exist.")
    
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
    for i, line in enumerate(lines):
        line = line.strip()
        if line.startswith("avg-cpu"):
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
    # 假设所有设备的记录是按时间顺序排列的
    unique_devices = iostat_device_df['Device'].unique()
    if len(unique_devices) == 0:
        num_records = 0
    else:
        num_records = len(iostat_device_df) // len(unique_devices)
    if num_records > 0 and start_time:
        iostat_device_df['Timestamp'] = pd.date_range(start=start_time, periods=num_records, freq=f'{sampling_interval}s').repeat(len(unique_devices))
    else:
        iostat_device_df['Timestamp'] = pd.date_range(start=datetime.now(), periods=0, freq=f'{sampling_interval}s').repeat(len(unique_devices))
    iostat_device_df.set_index('Timestamp', inplace=True)

    return iostat_cpu_df, iostat_device_df

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
        if line.startswith("/dev/"):
            data = line.strip().split()
            if len(data) < 5:
                continue
            try:
                size_str = data[1]
                used_str = data[2]
                avail_str = data[3]
                use_percent_str = data[4]

                # 移除所有非数字和小数点字符
                size = float(re.sub(r'[^\d.]', '', size_str))
                used = float(re.sub(r'[^\d.]', '', used_str))
                avail = float(re.sub(r'[^\d.]', '', avail_str))
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
    """
    解析pidstat日志文件
    """
    with open(log_path, 'r') as f:
        lines = f.readlines()

    cpu_data = []
    mem_data = []
    current_section = None

    # 从文件名提取日期，作为初始日期
    # 假设文件名格式: pidstat_YYYYMMDD_HHMMSS.log
    match = re.search(r'_(\d{8})_(\d{6})\.log$', log_path)
    if match:
        date_str = match.group(1)  # '20241217'
        time_str = match.group(2)  # '222151'
        try:
            start_date = datetime.strptime(date_str, "%Y%m%d").date()  # date对象，不包括时间
        except ValueError as ve:
            print(f"Error parsing start date: {ve}")
            start_date = datetime.now().date()
    else:
        print("Cannot extract date from filename, use current date.")
        start_date = datetime.now().date()

    # 用于跨日检测
    last_time_of_day = None
    current_date = start_date

    # 正则匹配行首时间，例如：22:21:54
    time_pattern = re.compile(r'^(\d{2}):(\d{2}):(\d{2})')

    for i in range(len(lines)):
        line = lines[i].strip()
        # 检查是否以时间戳开头 (HH:MM:SS)
        time_match = time_pattern.match(line)
        if time_match:
            timestamp_str = time_match.group(0)  # 'HH:MM:SS'
            headers = line.split()[1:]
            # 判断是CPU还是内存监控
            if 'minflt/s' in headers:
                current_section = 'mem'
            else:
                current_section = 'cpu'

            # 下一行是数据行
            if i + 1 < len(lines):
                data_line = lines[i + 1].strip()
                data = data_line.split()
                if current_section == 'cpu':
                    if len(data) >= 9:
                        # 处理可能的命令名称包含空格
                        command = ' '.join(data[9:]) if len(data) > 9 else data[8]
                        try:
                            # 解析当前时间
                            hh, mm, ss = map(int, timestamp_str.split(':'))
                            current_time_of_day = (hh, mm, ss)

                            # 检测是否跨日
                            if last_time_of_day is not None:
                                if current_time_of_day < last_time_of_day:
                                    # 日期加1
                                    current_date = current_date + timedelta(days=1)

                            last_time_of_day = current_time_of_day

                            # 构造datetime对象
                            timestamp = datetime(
                                current_date.year, current_date.month, current_date.day,
                                hh, mm, ss
                            )

                            entry = {
                                'Timestamp': timestamp,
                                'UID': data[1],
                                'PID': int(data[2]),
                                '%usr': float(data[3]),
                                '%system': float(data[4]),
                                '%guest': float(data[5]),
                                '%wait': float(data[6]),
                                '%CPU': float(data[7]),
                                'CPU': int(data[8]),
                                'Command': command
                            }
                            cpu_data.append(entry)
                        except ValueError as ve:
                            print(f"Error parsing CPU data line: {ve}")
                elif current_section == 'mem':
                    if len(data) >= 8:
                        # 处理可能的命令名称包含空格
                        command = ' '.join(data[7:]) if len(data) > 7 else data[6]
                        try:
                            # 解析当前时间
                            hh, mm, ss = map(int, timestamp_str.split(':'))
                            current_time_of_day = (hh, mm, ss)

                            # 检测是否跨日
                            if last_time_of_day is not None:
                                if current_time_of_day < last_time_of_day:
                                    # 日期加1
                                    current_date = current_date + timedelta(days=1)

                            last_time_of_day = current_time_of_day

                            # 构造datetime对象
                            timestamp = datetime(
                                current_date.year, current_date.month, current_date.day,
                                hh, mm, ss
                            )

                            entry = {
                                'Timestamp': timestamp,
                                'UID': data[1],
                                'PID': int(data[2]),
                                'minflt/s': float(data[3]),
                                'majflt/s': float(data[4]),
                                'VSZ': int(data[5]),
                                'RSS': int(data[6]),
                                '%MEM': float(data[7]),
                                'Command': command
                            }
                            mem_data.append(entry)
                        except ValueError as ve:
                            print(f"Error parsing memory data line: {ve}")

    # 创建DataFrame
    pidstat_cpu_df = pd.DataFrame(cpu_data)
    if not pidstat_cpu_df.empty:
        pidstat_cpu_df.set_index('Timestamp', inplace=True)

    pidstat_mem_df = pd.DataFrame(mem_data)
    if not pidstat_mem_df.empty:
        pidstat_mem_df.set_index('Timestamp', inplace=True)

    return pidstat_cpu_df, pidstat_mem_df

def parse_stress_test_log(log_path):
    """
    解析stress_test日志文件
    """
    elapsed_times = []
    throughput = []

    with open(log_path, 'r') as f:
        for line in f:
            match = re.search(r'\[Stats\] Elapsed: (\d+)s .* Throughput: (\d+) ops/s', line)
            if match:
                elapsed = int(match.group(1))
                th = int(match.group(2))
                elapsed_times.append(elapsed)
                throughput.append(th)
    
    stress_test_df = pd.DataFrame({
        'Elapsed Time (s)': elapsed_times,
        'Throughput (ops/s)': throughput
    })

    return stress_test_df

def plot_cpu_usage(vmstat_df, iostat_cpu_df, pidstat_cpu_df):
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
    plt.savefig('./charts/cpu_usage.png')
    plt.show()

def plot_memory_usage(vmstat_df, pidstat_mem_df):
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
    plt.savefig('./charts/memory_usage.png')
    plt.show()

def plot_disk_io(iostat_device_df):
    """
    绘制磁盘IO图表
    """
    plt.figure(figsize=(14, 7))

    # 以sdc设备为例
    sdc_df = iostat_device_df[iostat_device_df['Device'] == 'sdc']

    if not sdc_df.empty:
        plt.plot(sdc_df.index, sdc_df['rkB/s'], label='Read KB/s', color='blue')
        plt.plot(sdc_df.index, sdc_df['wkB/s'], label='Write KB/s', color='orange')
        plt.plot(sdc_df.index, sdc_df['%util'], label='%util', color='green')

    plt.xlabel('Time')
    plt.ylabel('Disk IO (KB/s) / %util')
    plt.title('Disk IO Over Time (Device: sdc)')
    plt.legend()
    plt.tight_layout()
    plt.savefig('./charts/disk_io.png')
    plt.show()

def plot_disk_usage(disk_df):
    """
    绘制磁盘使用情况图表
    """
    # print(f"Plotting Disk Usage from {disk_df.index.min()} to {disk_df.index.max()}")  
    
    plt.figure(figsize=(14, 7))

    plt.plot(disk_df.index, disk_df['Used'], label='Used (G)', color='blue')
    plt.plot(disk_df.index, disk_df['Avail'], label='Available (G)', color='orange')
    plt.plot(disk_df.index, disk_df['Use%'], label='Use% (%)', color='green')

    plt.xlabel('Time')
    plt.ylabel('Disk Usage (G) / Usage (%)')
    plt.title('Disk Usage Over Time')
    plt.legend()
    plt.tight_layout()
    plt.savefig('./charts/disk_usage.png')
    plt.show()

def plot_process_metrics(pidstat_cpu_df, pidstat_mem_df):
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
    plt.savefig('./charts/process_metrics.png')
    plt.show()

def plot_stress_test_throughput(df):
    """
    绘制stress_test的吞吐量图表
    """
    plt.figure(figsize=(14, 7))
    plt.plot(df['Elapsed Time (s)'], df['Throughput (ops/s)'], label='Total Throughput (ops/s)', color='blue')
    plt.xlabel('Elapsed Time (s)')
    plt.ylabel('Throughput (ops/s)')
    plt.title('Stress Test Throughput Over Time')
    plt.legend()
    plt.tight_layout()
    plt.savefig('./charts/stress_test_throughput.png')
    plt.show()

def main():
    # 设置日志文件路径
    log_dir = "./logs"
    # vmstat_log = os.path.join(log_dir, "vmstat_20241217_222151.log")
    # iostat_log = os.path.join(log_dir, "iostat_20241217_222151.log")
    # disk_usage_log = os.path.join(log_dir, "disk_usage_20241217_222151.log")
    # pidstat_log = os.path.join(log_dir, "pidstat_20241217_222151.log")
    # stress_test_log = os.path.join(log_dir,"stress_test_20241217_222151.log")
    
        # 自动获取日志文件
    log_files = get_log_files(log_dir)
    
    if not log_files:
        print("No valid log files found. Exiting.")
        return
    
    vmstat_log = log_files.get('vmstat')
    iostat_log = log_files.get('iostat')
    disk_usage_log = log_files.get('disk_usage')
    pidstat_log = log_files.get('pidstat')
    stress_test_log = log_files.get('stress_test')
    
    # 提取开始时间
    start_time = extract_start_time(vmstat_log)
    # print(f"Extracted start_time: {start_time}") 

    # 解析日志文件
    print("Parsing vmstat log...")
    vmstat_df = parse_vmstat(vmstat_log, sampling_interval=60, start_time=start_time)

    print("Parsing iostat log...")
    iostat_cpu_df, iostat_device_df = parse_iostat(iostat_log, sampling_interval=60, start_time=start_time)

    print("Parsing disk_usage log...")
    disk_df = parse_disk_usage(disk_usage_log, sampling_interval=300, start_time=start_time)

    print("Parsing pidstat log...")
    pidstat_cpu_df, pidstat_mem_df = parse_pidstat(pidstat_log, start_time=start_time)
    
    print("Parsing stress test log...")
    stress_test_df = parse_stress_test_log(stress_test_log)

    # 检查DataFrame是否为空
    if vmstat_df.empty:
        print("vmstat_df is empty. Please check the vmstat log file format.")
    if iostat_cpu_df.empty:
        print("iostat_cpu_df is empty. Please check the iostat log file format.")
    if iostat_device_df.empty:
        print("iostat_device_df is empty. Please check the iostat log file format.")
    if disk_df.empty:
        print("disk_df is empty. Please check the disk_usage log file format.")
    if pidstat_cpu_df.empty:
        print("pidstat_cpu_df is empty. Please check the pidstat log file format.")
    if pidstat_mem_df.empty:
        print("pidstat_mem_df is empty. Please check the pidstat log file format.")
    if stress_test_df.empty:
        print("stress_test_df is empty. Please check the stress test log file format.")
        
    # 生成图表
    if not vmstat_df.empty and not iostat_cpu_df.empty and not pidstat_cpu_df.empty:
        print("Generating CPU usage chart...")
        plot_cpu_usage(vmstat_df, iostat_cpu_df, pidstat_cpu_df)

    if not vmstat_df.empty and not pidstat_mem_df.empty:
        print("Generating memory usage chart...")
        plot_memory_usage(vmstat_df, pidstat_mem_df)

    if not iostat_device_df.empty:
        print("Generating Disk IO chart...")
        plot_disk_io(iostat_device_df)

    if not disk_df.empty:
        print("Generating Disk Usage chart...")
        plot_disk_usage(disk_df)

    if not pidstat_cpu_df.empty or not pidstat_mem_df.empty:
        print("Generating Process CPU and Memory Usage chart...")
        plot_process_metrics(pidstat_cpu_df, pidstat_mem_df)
        
    if not stress_test_df.empty:
        plot_stress_test_throughput(stress_test_df)

    print("All charts have been generated and saved in the current directory.")

if __name__ == "__main__":
    main()
