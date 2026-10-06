#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
只用 api.github.com 完成全部 GitHub 操作（因为 github.com 网页被墙、API 却通）。

用法:
    python3 scripts/gh.py push   <token> [仓库名]   # 建仓库 + 上传全部文件
    python3 scripts/gh.py run    <token> [仓库名]   # 触发工具链 workflow
    python3 scripts/gh.py status <token> [仓库名]   # 看 workflow 运行状态
    python3 scripts/gh.py fetch  <token> [仓库名]   # 下载构建产物 exe

令牌权限只需要 repo（或细粒度 Contents: Read and write + Actions: Read and write）。
"""
import base64, json, os, sys, time, urllib.error, urllib.request

API = "https://api.github.com"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKIP_DIRS = {".git", "build", "__pycache__"}
SKIP_FILES = {".gitignore"} if False else set()

def call(method, path, token, body=None, raw=False):
    req = urllib.request.Request(API + path, method=method)
    req.add_header("Authorization", "Bearer " + token)
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("User-Agent", "wince-carlife-agent")
    data = None
    if body is not None:
        data = json.dumps(body).encode()
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, data, timeout=60) as r:
            payload = r.read()
            return r.status, (payload if raw else (json.loads(payload) if payload else {}))
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace")[:400]
        return e.code, {"__error__": detail}

def collect_files():
    out = []
    for dp, dns, fns in os.walk(ROOT):
        dns[:] = [d for d in dns if d not in SKIP_DIRS]
        for fn in fns:
            if fn in SKIP_FILES:
                continue
            full = os.path.join(dp, fn)
            rel = os.path.relpath(full, ROOT).replace(os.sep, "/")
            out.append((rel, full))
    return sorted(out)

def push(token, repo):
    st, me = call("GET", "/user", token)
    if st != 200:
        sys.exit(f"❌ 令牌无效或无权访问: {st} {me}")
    owner = me["login"]
    print(f"账号: {owner}")

    st, r = call("GET", f"/repos/{owner}/{repo}", token)
    if st == 404:
        st, r = call("POST", "/user/repos", token,
                     {"name": repo, "private": True,
                      "description": "WinCE CarLife HU client"})
        if st not in (200, 201):
            sys.exit(f"❌ 建仓库失败: {st} {r}")
        print(f"✅ 已创建仓库 {owner}/{repo}（私有）")
        base_sha = None
    else:
        print(f"✅ 仓库已存在 {owner}/{repo}")
        st2, br = call("GET", f"/repos/{owner}/{repo}/git/ref/heads/{r.get('default_branch','main')}", token)
        base_sha = br.get("object", {}).get("sha") if st2 == 200 else None
        if base_sha:
            print(f"   已有提交 {base_sha[:8]}，将作为父提交")

    # ── 空仓库特殊处理 ──
    # GitHub 的 Git Data API 在"零提交"的仓库上建 blob 会返回 409
    # ("Git Repository is empty")。必须先用 Contents API 落一个文件点火。
    if not base_sha:
        branch = r.get("default_branch") or "main"
        st0, br0 = call("GET", f"/repos/{owner}/{repo}/git/ref/heads/{branch}", token)
        if st0 != 200:
            print("   仓库还没有任何提交 —— 用 Contents API 点火 …")
            st1, b1 = call("PUT", f"/repos/{owner}/{repo}/contents/README.md", token,
                           {"message": "init: bootstrap repository",
                            "content": base64.b64encode(
                                "# wince-carlife\n\nWinCE 车机端 CarLife 客户端（开发中）\n".encode()
                            ).decode()})
            if st1 not in (200, 201):
                sys.exit(f"❌ 点火失败: {st1} {b1}")
            print("   ✅ 已点火（产生第一个提交）")
            st0, br0 = call("GET", f"/repos/{owner}/{repo}/git/ref/heads/{branch}", token)
            if st0 != 200:
                sys.exit(f"❌ 取分支失败: {st0} {br0}")
        base_sha = br0["object"]["sha"]
        print(f"   基准提交 {base_sha[:8]}")

    files = collect_files()
    print(f"待上传 {len(files)} 个文件 …")
    tree = []
    for rel, full in files:
        with open(full, "rb") as f:
            content = base64.b64encode(f.read()).decode()
        st, b = call("POST", f"/repos/{owner}/{repo}/git/blobs", token,
                     {"content": content, "encoding": "base64"})
        if st not in (200, 201):
            sys.exit(f"❌ 上传 {rel} 失败: {st} {b}")
        tree.append({"path": rel, "mode": "100644", "type": "blob", "sha": b["sha"]})
    print(f"   {len(tree)} 个 blob 上传完成")

    body = {"tree": tree}
    if base_sha:
        body["base_tree"] = base_sha
    st, t = call("POST", f"/repos/{owner}/{repo}/git/trees", token, body)
    if st not in (200, 201):
        sys.exit(f"❌ 建 tree 失败: {st} {t}")

    cbody = {"message": "wince-carlife: 初始化（工具链 workflow + M1 骨架 + 协议文档）", "tree": t["sha"]}
    if base_sha:
        cbody["parents"] = [base_sha]
    st, c = call("POST", f"/repos/{owner}/{repo}/git/commits", token, cbody)
    if st not in (200, 201):
        sys.exit(f"❌ 建 commit 失败: {st} {c}")

    if base_sha:
        br_name = r.get("default_branch") or "main"
        st, _ = call("PATCH", f"/repos/{owner}/{repo}/git/refs/heads/{br_name}",
                     token, {"sha": c["sha"]})
    else:
        st, _ = call("POST", f"/repos/{owner}/{repo}/git/refs", token,
                     {"ref": "refs/heads/main", "sha": c["sha"]})
    if st not in (200, 201):
        sys.exit(f"❌ 建分支失败: {st}")
    print(f"✅ 完成！仓库地址 https://github.com/{owner}/{repo}")
    print(f"   下一步: python3 scripts/gh.py run {token[:6]}… {repo}")

def run(token, repo, want=None):
    """触发一个 workflow。want 可以是名称片段（如 "2" 或 "app"）；
    不给就触发列表里第一个（⚠ 第一个是工具链，很容易点错，建议显式给）。"""
    owner = call("GET", "/user", token)[1]["login"]
    st, r = call("GET", f"/repos/{owner}/{repo}/actions/workflows", token)
    if st != 200:
        sys.exit(f"❌ 取工作流列表失败 {st} {r}")
    wfs = r.get("workflows", [])
    if not wfs:
        sys.exit("❌ 仓库里没有 workflow")
    pick = None
    if want:
        for w in wfs:
            if want.lower() in w["name"].lower() or want == str(w["id"]):
                pick = w
                break
        if not pick:
            print("可选的工作流:")
            for w in wfs:
                print(f"   {w['id']}  {w['name']}  ({w['path']})")
            sys.exit(f"❌ 没找到匹配 [{want}] 的工作流")
    else:
        pick = wfs[0]
    st, _ = call("POST", f"/repos/{owner}/{repo}/actions/workflows/{pick['id']}/dispatches",
                 token, {"ref": "main"})
    if st not in (201, 204):
        sys.exit(f"❌ 触发失败 {st}")
    print(f"✅ 已触发『{pick['name']}』({pick['path']})")
    if "工具链" in pick["name"] or pick["name"].startswith("1."):
        print("   ⏱ 这是工具链构建，要 15-20 分钟。程序编译请用: run <token> <repo> 2")



def status(token, repo):
    owner = call("GET", "/user", token)[1]["login"]
    st, r = call("GET", f"/repos/{owner}/{repo}/actions/runs?per_page=5", token)
    if st != 200:
        sys.exit(f"❌ {st} {r}")
    if not r.get("workflow_runs"):
        print("还没有运行记录"); return
    for w in r["workflow_runs"]:
        print(f"  [{w['status']:>12}] {w['name']}  ({w['created_at'][:16]})")
        print(f"      {w['html_url']}")
        if w["status"] == "completed" and w["conclusion"] != "success":
            print(f"      ❌ 结论: {w['conclusion']}")

def fetch(token, repo):
    """下载最新的 wince-exe 产物并解压到 build/"""
    import zipfile
    owner = call("GET", "/user", token)[1]["login"]
    st, r = call("GET", f"/repos/{owner}/{repo}/actions/artifacts?per_page=50", token)
    if st != 200:
        sys.exit(f"❌ {st} {r}")
    arts = [a for a in r.get("artifacts", []) if not a["expired"]]
    named = [a for a in arts if a["name"] == "wince-exe"]
    if named:
        arts = named
    if not arts:
        print("还没有产物（工具链 workflow 可能还没跑完）")
        return
    a = max(arts, key=lambda x: x.get("created_at", ""))
    print(f"取产物: {a['name']}  创建于 {a['created_at'][:19]}  {a['size_in_bytes']} 字节")
    st, blob = call("GET", f"/repos/{owner}/{repo}/actions/artifacts/{a['id']}/zip",
                    token, raw=True)
    if st != 200:
        sys.exit(f"❌ 下载失败: {st}")
    outdir = os.path.join(ROOT, "build")
    os.makedirs(outdir, exist_ok=True)
    zpath = os.path.join(outdir, "wince-exe.zip")
    with open(zpath, "wb") as f:
        f.write(blob)
    with zipfile.ZipFile(zpath) as z:
        names = z.namelist()
        z.extractall(outdir)
    print(f"✅ 已解压到 {outdir}/")
    for n in names:
        full = os.path.join(outdir, n)
        if os.path.exists(full):
            print(f"     {n}  ({os.path.getsize(full)} 字节)")


def logs(token, repo):
    """抓最近一次运行里失败任务的日志尾部，用于诊断"""
    owner = call("GET", "/user", token)[1]["login"]
    st, r = call("GET", f"/repos/{owner}/{repo}/actions/runs?per_page=5", token)
    if st != 200:
        sys.exit(f"❌ {st} {r}")
    for w in r.get("workflow_runs", []):
        st, jobs = call("GET", f"/repos/{owner}/{repo}/actions/runs/{w['id']}/jobs", token)
        for j in jobs.get("jobs", []):
            print(f"── 运行 {w['id']} / 任务 {j['name']}  [{j.get('conclusion')}] ──")
            for s in j.get("steps", []):
                ic = {"success": "✅", "failure": "❌", "skipped": "⏭"}.get(s.get("conclusion"), "  ")
                print(f"   {ic} {s['name']}")
            if j.get("conclusion") == "failure":
                st2, blob = call("GET", f"/repos/{owner}/{repo}/actions/jobs/{j['id']}/logs",
                                 token, raw=True)
                if st2 == 200:
                    text = blob.decode("utf-8", "replace")
                    print("   ── 日志末尾 40 行 ──")
                    for line in text.splitlines()[-40:]:
                        print("   " + line[:180])
                else:
                    print(f"   （抓日志失败 {st2}）")
        if w.get("conclusion") == "success":
            break


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd, token = sys.argv[1], sys.argv[2]
    repo = sys.argv[3] if len(sys.argv) > 3 else "wince-carlife"
    {"push": push, "run": run, "status": status, "fetch": fetch, "logs": logs}.get(cmd, lambda *a: sys.exit(f"未知命令 {cmd}"))(token, repo)
