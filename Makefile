# WinCE 6.0 ARM 应用 —— 用 CeGCC (arm-mingw32ce) 交叉编译
#
# 产出两个独立程序（各自有自己的 WinMain，所以不能合并链接）：
#   build/WinCE-CarLifeHU.exe   主程序骨架（只用 coredll）
#   build/WinCE-NetProbe.exe    网络探测（额外链 ws2）
CROSS  ?= arm-mingw32ce-
CC     := $(CROSS)gcc

BUILD  := build

CFLAGS  := -Os -Wall -DUNICODE -D_UNICODE -Isrc
# -mwindows : PE 子系统 = WINDOWS_CE_GUI（车机只认 GUI 子系统）
LDFLAGS := -mwindows -s

MAIN_EXE  := $(BUILD)/WinCE-CarLifeHU.exe
PROBE_EXE := $(BUILD)/WinCE-NetProbe.exe

.PHONY: all main probe check clean

all: main probe

main: $(MAIN_EXE)
	@echo "==> 主程序: $(MAIN_EXE)"
	@ls -l $(MAIN_EXE)

# ⚠ 网络探测必须链 -lws2 —— 那是 WinCE 的 ws2.dll（已真机实证存在）。
#   绝不能写 -lws2_32：那会导入桌面的 WS2_32.dll，车机上没有，程序直接起不来。
probe: $(PROBE_EXE)
	@echo "==> 探测工具: $(PROBE_EXE)"
	@ls -l $(PROBE_EXE)

$(BUILD)/main.o: src/main.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/netprobe.o: src/netprobe.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(MAIN_EXE): $(BUILD)/main.o
	$(CC) $(LDFLAGS) -o $@ $^ -lcoredll

$(PROBE_EXE): $(BUILD)/netprobe.o
	$(CC) $(LDFLAGS) -o $@ $^ -lcoredll -lws2

$(BUILD):
	mkdir -p $(BUILD)

# 自检：把编译器实际报的名字打出来（CI 里用来确认工具链真的可用）
check:
	@$(CC) --version | head -1

clean:
	rm -rf $(BUILD)
