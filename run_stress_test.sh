#!/usr/bin/env bash

TEST_DIR="./KVStore"
LOG_DIR="$TEST_DIR/logs"
BIN_DIR="$TEST_DIR/bin"

mkdir -p "$LOG_DIR"

rm -f "$LOG_DIR"/*.log

# 获取当前日期时间
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")

# 启动资源监控工具，将输出重定向到相应的日志文件
# 每60秒采样一次
echo "Starting resource monitors..."

# vmstat监控CPU、内存、swap等系统级指标
vmstat 60 > "$LOG_DIR/vmstat_$TIMESTAMP.log" 2>&1 &
VMSTAT_PID=$!

# iostat监控磁盘IO性能指标
iostat -x 60 > "$LOG_DIR/iostat_$TIMESTAMP.log" 2>&1 &
IOSTAT_PID=$!

PIDSTAT_LOG="$LOG_DIR/pidstat_$TIMESTAMP.log"

# 定时记录磁盘使用情况，每5分钟执行一次df命令
(
    while true; do
        df -h "$TEST_DIR"
        sleep 300
    done
) > "$LOG_DIR/disk_usage_$TIMESTAMP.log" 2>&1 &
DISK_USAGE_PID=$!

echo "Starting stress test..."
# 启动压力测试程序并在后台运行
$BIN_DIR/stress_test > "$LOG_DIR/stress_test_$TIMESTAMP.log" 2>&1 &

STRESS_PID=$!

# 等待几秒钟以确保stress_test启动成功，然后使用pidstat监控此进程
sleep 5

pidstat -r -u 60 -p $STRESS_PID > "$PIDSTAT_LOG" 2>&1 &
PIDSTAT_PID=$!

echo "stress_test (PID: $STRESS_PID) and resource monitoring are running..."

# 等待压力测试结束
wait $STRESS_PID

echo "Stress test completed. Stopping resource monitors..."

# 终止所有监控进程
kill $VMSTAT_PID
kill $IOSTAT_PID
kill $PIDSTAT_PID
kill $DISK_USAGE_PID

echo "All done. Logs are in $LOG_DIR."
