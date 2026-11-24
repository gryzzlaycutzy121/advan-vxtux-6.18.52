#!/usr/bin/env python3
# verify_typec_fix.py — verifikasi fix sc27xx_typec dari artifact CI build-modules-verify.
# Bandingkan tabel .rodata sc27xx_typec.ko (3 entri x 10 u32) dengan ground truth stock.
import struct, subprocess, sys, os, zipfile, json, urllib.request, tempfile, shutil

RUN = sys.argv[1] if len(sys.argv) > 1 else "45477422321"
REPO = "gryzzlaycutzy121/advan-vxtux-6.18.52"
GH = "C:/Users/Administrator/AppData/Local/Temp/ghx/bin/gh.exe"
WORK = "E:/vxtux_build_repo"

EXPECT = {
    "sc2730": [0x2, 5, 0, 0x0c, 0x10, 0x04, 1, 2, 0x1f, 0x3ff],
    "sc2721": [0x1, 11, 6, 0x00, 0x04, 0x08, 0x20, 0x40, 0x0f, 0x7f],
    "ump9620": [0x3, 1, 11, 0x0c, 0x10, 0x04, 1, 2, 0x1f, 0x3ff],
}
FIELDS = ["type","cc1_shift","cc2_shift","state_en_off","irq_clr_off","mode_ctrl_off","state_en_bit0","state_en_bit1","state_mask","irq_pending_mask"]

def gh(args):
    r = subprocess.run([GH]+args, capture_output=True)
    return r.stdout, r.stderr, r.returncode

def gh_text(args):
    r = subprocess.run([GH]+args, capture_output=True, text=True)
    return r.stdout, r.stderr, r.returncode

def get_artifact(run):
    out,_,_ = gh_text(["api", f"repos/{REPO}/actions/runs/{run}/artifacts"])
    arts = json.loads(out)
    for a in arts["artifacts"]:
        if a["name"] == "built-kos":
            return a["id"], a["archive_download_url"]
    return None, None

def download(url, dest):
    # gh api output biner langsung ke file (tanpa decode teks)
    try:
        out, err, rc = gh(["api", f"repos/{REPO}/actions/artifacts/{url}/zip"])
        if rc == 0:
            open(dest,"wb").write(out)
            return True
        print(f"gh api fail rc={rc}: {err[:200]}")
        return False
    except Exception as e:
        print(f"download fail: {e}")
        return False

def rodata_table(ko_path):
    with open(ko_path,"rb") as f: data = f.read()
    assert data[:4] == b"\x7fELF", "not ELF"
    e_shoff = struct.unpack_from("<Q", data, 0x28)[0]
    e_shentsize = struct.unpack_from("<H", data, 0x3A)[0]
    e_shnum = struct.unpack_from("<H", data, 0x3C)[0]
    e_shstrndx = struct.unpack_from("<H", data, 0x3E)[0]
    shstr_off = struct.unpack_from("<Q", data, e_shoff + e_shstrndx*e_shentsize + 0x18)[0]
    shstr = data[shstr_off:]
    ro = None
    for i in range(e_shnum):
        off = e_shoff + i*e_shentsize
        name_off = struct.unpack_from("<I", data, off)[0]
        name = shstr[name_off:shstr.find(b"\0", name_off)].decode()
        if name == ".rodata":
            sh_offset = struct.unpack_from("<Q", data, off + 0x18)[0]
            sh_size = struct.unpack_from("<Q", data, off + 0x20)[0]
            ro = data[sh_offset:sh_offset+sh_size]
    if ro is None: raise Exception("no .rodata")
    return {n: list(struct.unpack_from("<10I", ro, a)) for n,a in [("sc2730",0x5a8),("sc2721",0x5d0),("ump9620",0x5f8)]}

def main():
    aid, url = get_artifact(RUN)
    print(f"artifact built-kos id={aid}")
    if not aid: print("NO ARTIFACT"); return 1
    zpath = os.path.join(WORK, f"_typec_verify_{RUN}.zip")
    if not download(aid, zpath):
        print("download gagal"); return 1
    with zipfile.ZipFile(zpath) as z:
        names = [n for n in z.namelist() if "sc27xx_typec.ko" in n]
        print("ko in artifact:", names)
        if not names: return 1
        z.extract(names[0], os.path.join(WORK, "_typec_verify"))
        ko = os.path.join(WORK, "_typec_verify", names[0])
        tbl = rodata_table(ko)
        ok = True
        for name, exp in EXPECT.items():
            got = tbl[name]
            diffs = [f"{FIELDS[i]}={got[i]:#x} (eks {exp[i]:#x})" for i in range(10) if got[i]!=exp[i]]
            status = "OK" if not diffs else "SALAH"
            if diffs: ok = False
            print(f"{name}: {status}" + (f" — {diffs}" if diffs else ""))
        print("VERDICT:", "PASS — semua 30 nilai cocok ground truth" if ok else "FAIL — masih ada nilai salah")
        return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
