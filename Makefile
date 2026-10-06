# WinCE 6.0 ARM 应用 —— CeGCC (arm-mingw32ce) 构建
CROSS  ?= arm-mingw32ce-
CC     := $(CROSS)gcc

BUILD  := build
TARGET := $(BUILD)/WinCE-CarLifeHU.exe
SRCS   := $(wildcard src/*.c)
OBJS   := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))

# -mwindows : PE 子系统设为 WINDOWS_GUI（车机只认 GUI 子系统）
CFLAGS  := -Os -Wall -DUNICODE -D_UNICODE -Isrc
LDFLAGS := -mwindows -s

.PHONY: all clean check
all: $(TARGET)
	@echo "==> 产物: $(TARGET)"
	@ls -l $(TARGET)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) -lcoredll

$(BUILD):
	mkdir -p $(BUILD)

# 自检：确认编译器真的存在（CI 里用来早失败）
check:
	@$(CC) --version | head -1

clean:
	rm -rf $(BUILD)
