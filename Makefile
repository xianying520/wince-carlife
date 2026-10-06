# WinCE 6.0 ARM 应用 —— 用 CeGCC (arm-mingw32ce) 交叉编译
#
# 产出两个独立程序（各自有自己的 WinMain，所以不能合并链接）：
#   build/WinCE-CarLifeHU.exe   主程序骨架（只用 coredll）
#   build/WinCE-NetProbe.exe    网络探测（额外链 ws2）
CROSS  ?= arm-mingw32ce-
CC     := $(CROSS)gcc

BUILD  := build

CFLAGS  := -Os -Wall -DUNICODE -D_UNICODE -Isrc

# PE 子系统必须是 9(WINDOWS_CE_GUI)，否则车机不认。
# ⚠ 实测教训：CeGCC(ENLYZE 9.3.0) 不认桌面 MinGW 的 -mwindows
#   （报 "unrecognized command line option"）。
# CI 会先探测出可用组合，再通过 make LDFLAGS=... 覆盖这里。
LDFLAGS ?= -Wl,--subsystem,windowsce -s

# 导入库名同样可能因工具链而异，CI 探测后覆盖。
# 注意 NETLIB 必须是 WinCE 的 ws2（对应 ws2.dll），
# 绝不是桌面的 ws2_32（对应 WS2_32.dll，车机上不存在）。
CORELIB ?= -lcoredll
NETLIB  ?= -lws2

MAIN_EXE   := $(BUILD)/WinCE-CarLifeHU.exe
PROBE_EXE  := $(BUILD)/WinCE-NetProbe.exe
CLIENT_EXE := $(BUILD)/WinCE-CarLifeClient.exe

.PHONY: all main probe client check clean

all: main probe client

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

$(BUILD)/netprobe.o: src/netprobe.c src/carlife.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/huclient.o: src/huclient.c src/carlife.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/carlife.o: src/carlife.c src/carlife.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(MAIN_EXE): $(BUILD)/main.o
	$(CC) $(LDFLAGS) -o $@ $^ $(CORELIB)

$(PROBE_EXE): $(BUILD)/netprobe.o $(BUILD)/carlife.o
	$(CC) $(LDFLAGS) -o $@ $^ $(CORELIB) -lws2

# 主客户端：握手 + 视频初始化 + 收帧（也链 ws2）
client: $(CLIENT_EXE)
	@echo "==> 客户端: $(CLIENT_EXE)"
	@ls -l $(CLIENT_EXE)

$(CLIENT_EXE): $(BUILD)/huclient.o $(BUILD)/carlife.o
	$(CC) $(LDFLAGS) -o $@ $^ $(CORELIB) -lws2

$(BUILD):
	mkdir -p $(BUILD)

# 自检：把编译器实际报的名字打出来（CI 里用来确认工具链真的可用）
check:
	@$(CC) --version | head -1

clean:
	rm -rf $(BUILD)
