#!/usr/bin/env python3
"""查看最近一次 GitHub Actions 运行的结果，并把关键日志片段打出来。

用法:
    python3 scripts/ghrun.py <token> [日志里要抓的关键词 ...]

不带关键词时只打印步骤清单和失败点。带关键词时，把包含该关键词的那段
日志再打 40 行出来 —— 比拉整份日志省很多。
"""
import json
import sys
import urllib.request as urllib_request

REPO = "/repos/xianying520/wince-carlife"


class _NoAuthOnRedirect(urllib_request.HTTPRedirectHandler):
    """GitHub 的日志下载会 302 到另一台机器，跟着转的时候不能带上
    Authorization，否则那台机器会拒（认证头对不上签名）。"""
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        new = super().redirect_request(req, fp, code, msg, headers, newurl)
        if new:
            for k in list(new.headers):
                if k.lower() == "authorization":
                    del new.headers[k]
        return new


def api(token, path):
    r = urllib_request.Request("https://api.github.com" + path)
    r.add_header("Authorization", "Bearer " + token)
    r.add_header("User-Agent", "dsh")
    with urllib_request.urlopen(r, timeout=60) as x:
        return json.loads(x.read() or b"{}")


def fetch_log(token, job_id):
    r = urllib_request.Request(
        f"https://api.github.com{REPO}/actions/jobs/{job_id}/logs")
    r.add_header("Authorization", "Bearer " + token)
    r.add_header("User-Agent", "dsh")
    opener = urllib_request.build_opener(_NoAuthOnRedirect)
    with opener.open(r, timeout=180) as x:
        txt = x.read().decode("utf-8", "replace")
    return [l.split("Z ", 1)[-1] for l in txt.splitlines()]


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    token = sys.argv[1]
    keywords = sys.argv[2:]

    w = api(token, f"{REPO}/actions/runs?per_page=1")["workflow_runs"][0]
    print(f"  [{w['status']}] {w.get('conclusion') or ''}  {w['head_sha'][:8]}")
    if w["status"] != "completed":
        print("  （还在跑，等一会儿再看）")
        return 0

    ok = fail = 0
    for j in api(token, f"{REPO}/actions/runs/{w['id']}/jobs").get("jobs", []):
        for s in j.get("steps", []):
            c = s.get("conclusion")
            if c == "success":
                ok += 1
            if c == "failure":
                fail += 1
                print(f"  ❌ {s['name']}")
    print(f"  ── {ok} 成功 / {fail} 失败 ──")

    jid = api(token, f"{REPO}/actions/runs/{w['id']}/jobs")["jobs"][0]["id"]
    lines = fetch_log(token, jid)

    for kw in keywords:
        idx = [i for i, l in enumerate(lines) if kw in l]
        if not idx:
            print(f"  ── 没找到「{kw}」──")
            continue
        print(f"  ── {kw} ──")
        for l in lines[idx[-1]:idx[-1] + 40]:
            if l.strip():
                print("   ", l[:170])

    if fail and not keywords:
        for i, l in enumerate(lines):
            if "error:" in l or "❌ " in l or "undefined reference" in l:
                print("  ── 失败点 ──")
                for k in lines[max(0, i - 3):i + 8]:
                    if k.strip():
                        print("   ", k[:170])
                break
    return 0


if __name__ == "__main__":
    sys.exit(main())
