# WinCE 6.0 ARM 应用 —— 用 CeGCC (arm-mingw32ce) 交叉编译
#
# 四个独立程序，各自有自己的 WinMain，所以必须分别链接：
#   build/WinCE-CarLifeHU.exe     主程序骨架（只用 coredll）
#   build/WinCE-NetProbe.exe      网络/解码器/握手 全探测（额外链 ws2）
#   build/WinCE-CarLifeClient.exe 协议客户端：握手→视频初始化→收帧取证
#   build/WinCE-CarLifeView.exe   ⭐ 最终程序：显示手机画面 + 触屏回传
CROSS  ?= arm-mingw32ce-
CC     := $(CROSS)gcc

BUILD  := build

CFLAGS  := -Os -Wall -DUNICODE -D_UNICODE -Isrc

# PE 子系统必须是 9(WINDOWS_CE_GUI)，否则车机不认。
#
# ⚠ 实测教训（两轮 CI 换来的，别再走回头路）：
#   · -mwindows              → CeGCC 不认："unrecognized command line option"
#   · -Wl,--subsystem,windowsce → ld 不认："invalid subsystem type windowsce"
#   · 不加任何旗标            → ✅ 链接成功（CeGCC 的 arm-mingw32ce 目标
#                              本来就默认产出 WinCE 程序，不需要额外旗标）
# CI 仍会先探测一次再覆盖这里，以防工具链换版本后行为改变。
LDFLAGS ?= -s

# 导入库名同样可能因工具链而异，CI 探测后覆盖。
# 注意 NETLIB 必须是 WinCE 的 ws2（对应 ws2.dll），
# 绝不是桌面的 ws2_32（对应 WS2_32.dll，车机上不存在）。
CORELIB ?= -lcoredll
NETLIB  ?= -lws2

MAIN_EXE   := $(BUILD)/WinCE-CarLifeHU.exe
PROBE_EXE  := $(BUILD)/WinCE-NetProbe.exe
CLIENT_EXE := $(BUILD)/WinCE-CarLifeClient.exe
VIEWER_EXE := $(BUILD)/WinCE-CarLifeView.exe

.PHONY: all main probe client viewer check clean

all: main probe client viewer

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
	$(CC) $(LDFLAGS) -o $@ $^ $(CORELIB) $(NETLIB)

# 主客户端：握手 + 视频初始化 + 收帧（也链 ws2）
client: $(CLIENT_EXE)
	@echo "==> 客户端: $(CLIENT_EXE)"
	@ls -l $(CLIENT_EXE)

$(CLIENT_EXE): $(BUILD)/huclient.o $(BUILD)/carlife.o
	$(CC) $(LDFLAGS) -o $@ $^ $(CORELIB) $(NETLIB)

# ⭐ 最终程序：收帧 → nanojpeg 解码 → 显示 → 触摸回传
viewer: $(VIEWER_EXE)
	@echo "==> 最终程序: $(VIEWER_EXE)"
	@ls -l $(VIEWER_EXE)

$(BUILD)/viewer.o: src/viewer.c src/carlife.h src/display.h src/third_party/nanojpeg.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/display.o: src/display.c src/display.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# nanojpeg 是第三方源码（MIT），不套用项目自身的 -DUNICODE 等旗标，
# 并且关掉它自身的警告噪音 —— 我们不改第三方代码。
$(BUILD)/nanojpeg.o: src/third_party/nanojpeg.c | $(BUILD)
	$(CC) -Os -w -c $< -o $@

$(VIEWER_EXE): $(BUILD)/viewer.o $(BUILD)/carlife.o $(BUILD)/display.o $(BUILD)/nanojpeg.o
	$(CC) $(LDFLAGS) -o $@ $^ $(CORELIB) $(NETLIB)

$(BUILD):
	mkdir -p $(BUILD)

# 自检：把编译器实际报的名字打出来（CI 里用来确认工具链真的可用）
check:
	@$(CC) --version | head -1

clean:
	rm -rf $(BUILD)
