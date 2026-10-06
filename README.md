# wince-carlife — WinCE 车机端（CarLife 协议）

目标：写一个跑在 **WinCE 6.0 ARM** 车机上的程序，用 **CarLife 协议**去连接
安卓手机上的车机系统（vivo **Jovi InCar**），不需要任何硬件盒子。

## 为什么需要云端编译

CarLife 车机端要写 C/C++（协议栈 + H.264 解码 + 触摸回传），
手工拼 ARM 指令做不了这个量级，必须用真正的交叉编译器 **CeGCC (`arm-mingw32ce`)**。
CeGCC 没有官方预编译包，所以本仓库的 CI **自己编一个**，编完发布成 Release 资产，
之后每次构建直接下载（约 1 分钟）。

## 使用步骤（顺序很重要）

### 第 1 步：建仓库并推上去

```bash
git init
git add .
git commit -m "init"
git branch -M main
git remote add origin https://github.com/<你的用户名>/wince-carlife.git
git push -u origin main
```

### 第 2 步：先编工具链（只做一次）

进 **Actions** → 左侧选 **`1. Build CeGCC toolchain (ARM, run once)`** →
**Run workflow**。

> ⏱ **这一步要跑 30–60 分钟**，它在从头编译 binutils + GCC。
> 跑完后会在 Releases 里出现 `cegcc-arm` 标签，附带 `cegcc-arm-mingw32ce.tar.gz`。
>
> ❗ 如果这一步失败：把 **失败步骤的日志**贴给我。这步是整套东西的地基，
> 必须先过。

### 第 3 步：编应用

工具链 Release 出来之后，**`2. Build WinCE app`** 这个 workflow 会在每次 push 时
自动运行：下载工具链 → 编译 → 把 exe 作为 artifact 上传。
在 Actions 运行页底部的 **Artifacts** 里下载 `wince-exe`。

### 第 4 步：上车机验收

把 `WinCE-CarLifeHU.exe` 拷到 U 盘 → 车机文件管理器运行。

**这个程序会用一个真窗口显示车机信息**（分辨率 / WinCE 版本 / CPU / 内存）。
窗口用自绘，**故意不弹 MessageBox** —— 因为实测你这台车机只能显示一个弹窗，
真窗口才是正路。

**点一下屏幕或按任意键，程序退出。**

## 目录

```
.
├── .github/workflows/
│   ├── 1-build-toolchain.yml   # 编 CeGCC ARM 工具链（只跑一次，发 Release）
│   └── 2-build-app.yml         # 下工具链 → 编本程序 → 传 artifact
├── Makefile                    # arm-mingw32ce-gcc 构建规则
└── src/main.c                  # M1 骨架：真窗口 + 车机信息自绘
```

## 后续路线

| 阶段 | 内容 | 状态 |
|---|---|---|
| M0 | 无工具链产出 WinCE 程序、部署、单弹窗汇报 | ✅ 已跑通 |
| **M1** | **本仓库：真工具链 + 真窗口骨架** | **进行中** |
| M2 | USB 传输层（ADB / AOA / USB 网络 —— 待定） | 待定 |
| M3 | CarLife 协议（认证 + 六通道复用） | 规格已有 |
| M4 | H.264 解码 → DirectDraw | 待做 |
| M5 | 触摸回传 + 音频 | 待做 |
