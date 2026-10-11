#!/usr/bin/env python3
"""CT-01: audit every file name the game asks for against the retail USA disc file list.

Usage:  py -3 xbox/tools/filelist_check.py [--repo <root>] [--out <filenames.inc>] [--quiet]

Inputs (all in the repo, no game data needed):
  src/game/dvd.cpp             FileTblEntry FileTbl[] (the indexed table, dvd.cpp:7)
  xbox/tools/retail_filelist.txt  "<disc>\t<path relative to files/>\t<size>" from `dtk vfs ls`
  the hand-written tables below (STATIC_NAMES, DYNAMIC_PATTERNS, DEBUG_ONLY) with their source lines
  a literal-string scan of src/game, src/Sscrn, src/st1..4 as a cross-check that the tables are complete

Output: a report on stdout (exit 0 unless the inputs are missing) and xbox/port/dvd/filenames.inc,
the generated name -> retail path/disc table consumed by the DVD shim.

How the game resolves names (see xbox/docs/04-DATA.md "File resolution"):
  * cDvd::FileTblExistCheck (src/game/dvd.cpp:1261) turns every FileTbl[].name into a disc entry
    number with DVDConvertPathToEntrynum at boot; DvdRead(fileNo) (dvd.cpp:346) uses that number,
    DvdReadN(name) (dvd.cpp:365) converts the path again at request time (dvd.cpp:864).
  * DVDConvertPathToEntrynum is case-insensitive (src/lib/dvdfs.c:50 isSame() -> tolower), so
    "rel/st1_0.rel" finds "Rel/st1_0.rel"; the shim must compare case-insensitively too.
  * SET_DRS_NAME (src/game/read.cpp:107) overwrites the extension of a FileTbl name IN PLACE with
    "drs" before DvdReadN: the ".das" spellings of em/*.das in FileTbl are only templates, every disc
    (retail and debug, config/G4BE08/config.yml:18) carries em/*.drs.  Users: readEmData
    read.cpp:386 (e->dll != 0), ReadPlayerData read.cpp:652 (always), ReadWepData read.cpp:922.
"""

import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

LANGS = ("jpn", "eng", "ger", "fra", "esp", "ita")   # option.cpp:39 setLangExt3 / sscrn.cpp:103
STAGES = (1, 2, 3, 4)                                  # St1..St4 exist on retail; src/st1..st4

# ----------------------------------------------------------------------------------------------
# 1. Static names requested with DvdReadN / FileExistCheck / DC.setData / Sofdec.Initialize
#    (not through FileTbl).  (name as written in the code, "file:line", note)
# ----------------------------------------------------------------------------------------------
STATIC_NAMES = [
    ("etc/sizetbl.dat",    "src/game/dvd.cpp:1161",     "cDvd::SizeTableRead (disc spells it SizeTbl.dat)"),
    ("dummy.dat",          "src/game/datactrl.cpp:221", "cDataUnit dummy read (also :324)"),
    ("Etc/Ending.tpl",     "src/game/game.cpp:984",     "gameEnding() Rno0==5 still picture; retail ends through Movie/ending.sfd (r333.cpp:599) and has no Ending.tpl"),
    ("rel/Sscrn.rel",      "src/game/sscrn.cpp:82",     "sub screen REL"),
    ("SS/cmn/title.snd",   "src/game/title.cpp:118",    "title sound bank"),
    ("Font/common_j.fnt",  "src/game/mes.cpp:436",      "cMes::loadFont (also card.cpp:2163, FileExistCheck mes.cpp:372)"),
    ("Font/common_p.fnt",  "src/game/mes.cpp:438",      "cMes::loadFont (also card.cpp:2165)"),
    ("Font/system_j.fnt",  "src/game/mes.cpp:446",      "cMes::loadFont (FileExistCheck mes.cpp:381)"),
    ("Font/stage1_j.fnt",  "src/game/mes.cpp:470",      "FileExistCheck size probe"),
    ("movie/dmy.sfd",      "src/game/sofdec.cpp:512",   "fallback when a movie is missing; its own absence is handled (sofdec.cpp:513 returns 0)"),
    ("movie/e3_jpn.sfd",   "src/game/title.cpp:688",    "Sofdec.Initialize E3 trailer, JPN title state 5 only"),
    ("evd/r100s01.evd",    "src/st1/r100.cpp:411",      "r100_evtName[1]: dead row, readEvent() is only called with 0,3,4,5,7,8,9"),
    ("evd/r100s02.evd",    "src/st1/r100.cpp:411",      "r100_evtName[2]: dead row (see above)"),
    ("evd/r100s42.evd",    "src/st1/r100.cpp:411",      "r100_evtName[6]: dead row (see above)"),
    ("movie/demo0.sfd",    "src/game/title.cpp:723",    "Sofdec.Initialize attract movie"),
    ("movie/demo1.sfd",    "src/game/title.cpp:725",    "Sofdec.Initialize attract movie"),
    ("movie/opening.sfd",  "src/st1/r120.cpp:74",       "Sofdec.Initialize"),
    ("movie/r214_ev.sfd",  "src/st2/r217.cpp:509",      "Sofdec.Initialize"),
    ("movie/r229_ev.sfd",  "src/st2/r221.cpp:329",      "Sofdec.Initialize"),
    ("movie/ending.sfd",   "src/st3/r333.cpp:599",      "Sofdec.Initialize"),
    ("movie/r333_ev.sfd",  "src/st3/st3.cpp:266",       "Sofdec.Initialize"),
    ("movie/adaend_m.sfd", "src/st4/r40e.cpp:386",      "Sofdec.Initialize"),
    ("movie/adaend_c.sfd", "src/st4/r40e.cpp:389",      "Sofdec.Initialize"),
    ("etc/jpn/id101.eff",  "src/st1/r101.cpp:379",      "DC.setData (lang 0)"),
    ("etc/eng/id101.eff",  "src/st1/r101.cpp:381",      "DC.setData (lang != 0)"),
    ("etc/jpn/event001.uwf", "src/st1/r101.cpp:385",    "DC.setData (lang 0)"),
    ("etc/eng/event001.uwf", "src/st1/r101.cpp:387",    "DC.setData (lang != 0)"),
    ("etc/pl010a.bin",     "src/st3/r300.cpp:1478",     "DC.setData Ashley hands"),
    ("etc/pl010a.tpl",     "src/st3/r300.cpp:1479",     "DC.setData"),
    ("etc/pl010d.bin",     "src/st3/r300.cpp:1480",     "DC.setData"),
    ("etc/pl010e.bin",     "src/st3/r300.cpp:1481",     "DC.setData"),
    ("etc/pl01rh00.bin",   "src/st3/r300.cpp:1482",     "DC.setData"),
    ("etc/pl01lh00.bin",   "src/st3/r300.cpp:1483",     "DC.setData"),
    ("etc/pl050a.bin",     "src/st3/r300.cpp:1485",     "DC.setData"),
    ("etc/pl050a.tpl",     "src/st3/r300.cpp:1486",     "DC.setData"),
    ("etc/pl050d.bin",     "src/st3/r300.cpp:1487",     "DC.setData"),
    ("etc/pl050e.bin",     "src/st3/r300.cpp:1488",     "DC.setData"),
    ("etc/pl05rh00.bin",   "src/st3/r300.cpp:1489",     "DC.setData"),
    ("etc/pl05lh00.bin",   "src/st3/r300.cpp:1490",     "DC.setData"),
]
# etc/emleon00..07.esl, etc/omake00..02.esl: stage.cpp:137-139 EmList table, readEmList stage.cpp:243
STATIC_NAMES += [("etc/emleon%02d.esl" % i, "src/game/stage.cpp:137", "EmList table (readEmList)") for i in range(8)]
STATIC_NAMES += [("etc/omake%02d.esl" % i, "src/game/stage.cpp:139", "EmList table (readEmList)") for i in range(3)]
# SS/cmn/map_objN.dat: ss_map.cpp:2300-2351 one sprintf per map
STATIC_NAMES += [("SS/cmn/map_obj%s.dat" % s, "src/Sscrn/ss_map.cpp:2300", "map object model (one sprintf per map, :2300-2351)")
                 for s in ("1", "2a", "2b", "2c", "3a", "3b", "3c", "3d", "3e", "3f",
                           "4a", "4b", "4c", "4d", "4e", "4f", "4g")]
# SS/cmn/ss_ocNNN.dat: ss_term.cpp:638-644 partnerDataName tbl[24]
STATIC_NAMES += [("SS/cmn/ss_oc%s.dat" % s, "src/Sscrn/ss_term.cpp:644", "partnerDataName tbl[24]")
                 for s in ("101", "102", "103a", "103b", "104", "105", "106", "107", "108", "109", "110", "111",
                           "112", "201", "202", "203", "204", "205", "206", "301", "302", "303", "304", "305")]
# "SS/___/<name>" + setLangExt3(name+3) (option.cpp:39) / sscrnSetLanguage (sscrn.cpp:103): one per language.
LANG_NAMES = [
    ("SS/___/option.dat",  "src/game/read.cpp:219",      "OptionDataRead sprintf(\"SS/%3s/option.dat\", lang[])"),
    ("SS/___/title.dat",   "src/game/title.cpp:150",     "title_dat + setLangExt3"),
    ("SS/___/omk_t0.dat",  "src/game/title.cpp:859",     "omake_dat[12]='0' (Ada)"),
    ("SS/___/omk_t1.dat",  "src/game/title.cpp:859",     "omake_dat[12]='1' (Mercenaries)"),
    ("SS/___/omk_r0.dat",  "src/game/mercenaries.cpp:1191", "data_name + setLangExt3"),
    ("SS/___/omk_r1.dat",  "src/game/mercenaries.cpp:583",  "data_name + setLangExt3 (also :1037)"),
    ("SS/___/id400.dat",   "src/game/mercenaries.cpp:982",  "data_name + setLangExt3"),
    ("SS/___/id22c.dat",   "src/st2/r22c.cpp:1691",      "r22c_fname + setLangExt3"),
    ("SS/___/id333.dat",   "src/st3/r333.cpp:762",       "id_name + setLangExt3"),
    ("SS/___/result.dat",  "src/st3/r333.cpp:572",       "data_name + setLangExt3"),
    ("SS/___/ss_cmmn.dat", "src/game/sscrn.cpp:86",      "sscrnDataFilename"),
    ("SS/___/ss_pzzl.dat", "src/game/sscrn.cpp:92",      "sscrnDataFilename (also :623)"),
    ("SS/___/ss_cap.dat",  "src/Sscrn/ss_cap.cpp",       "sscrnDataFilename -> wk->filename (ss_cap.cpp:68)"),
    ("SS/___/ss_file.dat", "src/Sscrn/ss_file.cpp",      "sscrnDataFilename -> wk->filename (ss_file.cpp:292)"),
    ("SS/___/ss_item.dat", "src/Sscrn/ss_item.cpp",      "sscrnDataFilename -> wk->filename (ss_item.cpp:187)"),
    ("SS/___/ss_map.dat",  "src/Sscrn/ss_map.cpp",       "sscrnDataFilename -> wk->filename (ss_map.cpp:2398)"),
    ("SS/___/ss_shop.dat", "src/Sscrn/ss_shop.cpp",      "sscrnDataFilename -> wk->filename (ss_shop.cpp:407)"),
    ("SS/___/ss_term.dat", "src/Sscrn/ss_term.cpp",      "sscrnDataFilename -> wk->filename (ss_term.cpp:716)"),
]
LANG_NAMES += [("SS/___/chap%02d.dat" % c, "src/game/sce_com.cpp:749", "chap_data_name sprintf (chap06/07 at :745/:747)")
               for c in range(1, 8)]
# ss/cmn/save_<j|e|g|f|s|i>.dat: card.cpp:172 idpath[] / card.cpp:173 fileext[] = "jeegfsie" indexed by language
STATIC_NAMES += [("ss/cmn/save_%s.dat" % c, "src/game/card.cpp:172", "idpath[12] = fileext[lang] (\"jeegfsie\")")
                 for c in sorted(set("jeegfsie"))]
# Static names that are absent on the retail USA discs for a known reason (not a shim problem).
EXPECTED_ABSENT = {
    "etc/ending.tpl":     "debug ending screen; retail has no Ending.tpl",
    "movie/dmy.sfd":      "fallback only, absence handled by sofdec.cpp:513",
    "movie/e3_jpn.sfd":   "JPN-only E3 trailer",
    "etc/jpn/id101.eff":  "language 0 (jpn) only; USA discs ship etc/Eng only",
    "etc/jpn/event001.uwf": "language 0 (jpn) only; USA discs ship etc/Eng only",
    "evd/r100s01.evd":    "dead r100_evtName[] row, never loaded",
    "evd/r100s02.evd":    "dead r100_evtName[] row, never loaded",
    "evd/r100s42.evd":    "dead r100_evtName[] row, never loaded",
    "op/op04.das":        "stage 4 (St4 = Separate Ways / Mercenaries rooms) has no radio ops; retail ships op01..op03",
}
# FileTbl entries with no retail file: why (index -> reason).  Checked by grep: no DVD_READ(idx) /
# EmFileTbl / wep_data row names these indices.
FILETBL_NOTES = {
    "etc/moji8.tpl":  "debug-only: eprintf.cpp:325 debug overlay font, read only while debug_mode is set",
    "etc/title.das":  "debug-only: FileTbl[0x0b] is never read by index (grep DVD_READ/DvdRead), retail title data is SS/<lang>/title.dat",
    "etc/youdied.tpl": "debug-only: FileTbl[0x27] is never read by index; retail 'you died' lives in the room/core archives",
    "etc/thank.tpl":  "debug-only: FileTbl[0x28] is never read by index",
    "em/em06.das":    "unreferenced: FileTbl[0x1c] appears in no EmFileTbl row (read.cpp:272-343); retail has no em06.drs",
    "em/wep99.das":   "unreferenced: FileTbl[0x2d] appears in no wep_data row (read.cpp:727-797); retail has no wep99.drs",
}

# ----------------------------------------------------------------------------------------------
# 2. Dynamic sprintf patterns.  `regex` matches the retail path (case-insensitive) and `code` renders
#    the name the game would build from the match (code spelling) so filenames.inc can map it.
#    `expect` optionally enumerates names the code can build, to find expansions absent on retail.
# ----------------------------------------------------------------------------------------------
DYNAMIC_PATTERNS = [
    dict(fmt="st%x/r%03x.das", src="src/game/read.cpp:158", note="ReadAreaData room archive (yz2)",
         regex=r"st([0-9a-f])/r([0-9a-f]{3})\.das", code=lambda m: "st%s/r%s.das" % (m[0], m[1])),
    dict(fmt="st%x/r%03x_%02x.dat", src="src/game/block.cpp:662", note="room sub-block (DC.setData)",
         regex=r"st([0-9a-f])/r([0-9a-f]{3})_([0-9a-f]{2})\.dat", code=lambda m: "st%s/r%s_%s.dat" % m),
    dict(fmt="evd/r%03xs%02d.evd (+ r->s costume)", src="src/game/event.cpp:2280", note="EvtReadSub strstr(\"evd/\") + NameChange (event.cpp:2258) + DC.setData; 80 \"event/evd/..\" literals in src/game, src/st*",
         regex=r"evd/([rs][0-9a-f]{3}s[0-9]{2})\.evd", code=lambda m: "evd/%s.evd" % m),
    # em/<name>.das -> em/<name>.drs (read.cpp:107 SET_DRS_NAME) is not a free pattern: only FileTbl
    # names are rewritten, so it is applied per FileTbl entry in main() ("filetbl-drs" rows).
    dict(fmt="SS/%3s/option.dat", src="src/game/read.cpp:219", note="OptionDataRead, lang[] table read.cpp:216",
         regex=r"ss/([a-z]{3})/option\.dat", code=lambda m: "SS/%s/option.dat" % m),
    dict(fmt="Font/stage%1d_j.fnt", src="src/game/mes.cpp:461", note="cMes stage font (also :490)",
         regex=r"font/stage([0-9])_j\.fnt", code=lambda m: "Font/stage%s_j.fnt" % m,
         expect=["Font/stage%d_j.fnt" % s for s in STAGES]),
    dict(fmt="Font/event%1d_j.fnt", src="src/game/mes.cpp:465", note="cMes event font (also :502)",
         regex=r"font/event([0-9])_j\.fnt", code=lambda m: "Font/event%s_j.fnt" % m,
         expect=["Font/event%d_j.fnt" % s for s in STAGES]),
    dict(fmt="SS/___/f%02dA.tpl (A = 'a'+nxt)", src="src/Sscrn/ss_file.cpp:889", note="file screen page texture, setLangExt3 + name[10]=nxt+0x60",
         regex=r"ss/([a-z]{3})/f([0-9]{2})([a-z])\.tpl", code=lambda m: "SS/%s/f%s%s.tpl" % m),
    dict(fmt="SS/item/cap%02d.bin", src="src/Sscrn/ss_main.cpp:498", note="item exam (exam_id - 0xDB)",
         regex=r"ss/item/cap([0-9]{2})\.bin", code=lambda m: "SS/item/cap%s.bin" % m),
    dict(fmt="SS/item/cap%02d.tpl", src="src/Sscrn/ss_main.cpp:526", note="item exam texture",
         regex=r"ss/item/cap([0-9]{2})\.tpl", code=lambda m: "SS/item/cap%s.tpl" % m),
    dict(fmt="SS/item/idm%03x.bin", src="src/Sscrn/ss_main.cpp:500", note="item exam model (exam_id)",
         regex=r"ss/item/idm([0-9a-f]{3})\.bin", code=lambda m: "SS/item/idm%s.bin" % m),
    dict(fmt="SS/item/idm%03x.tpl", src="src/Sscrn/ss_main.cpp:528", note="item exam texture",
         regex=r"ss/item/idm([0-9a-f]{3})\.tpl", code=lambda m: "SS/item/idm%s.tpl" % m),
    dict(fmt="SS/cmn/ss_wep%02ld.dat", src="src/Sscrn/ss_model.cpp:71", note="weapon model on the sub screen (fixed names :74-144)",
         regex=r"ss/cmn/ss_wep([0-9]{2})\.dat", code=lambda m: "SS/cmn/ss_wep%s.dat" % m),
    dict(fmt="SS/cmn/itm%02x.bin", src="src/game/sce_at.cpp:936", note="item model picked up in-game",
         regex=r"ss/cmn/itm([0-9a-f]{2})\.bin", code=lambda m: "SS/cmn/itm%s.bin" % m),
    dict(fmt="SS/cmn/itm%02x.tpl", src="src/game/sce_at.cpp:937", note="item texture picked up in-game",
         regex=r"ss/cmn/itm([0-9a-f]{2})\.tpl", code=lambda m: "SS/cmn/itm%s.tpl" % m),
    dict(fmt="op/op%02d.das", src="src/Sscrn/ss_term.cpp:727", note="radio op message archive per stage",
         regex=r"op/op([0-9]{2})\.das", code=lambda m: "op/op%s.das" % m,
         expect=["op/op%02d.das" % s for s in STAGES]),
]

# ----------------------------------------------------------------------------------------------
# 3. Names only reachable in the debug build / SN host file system / in-game tools, never on a disc.
# ----------------------------------------------------------------------------------------------
DEBUG_ONLY = [
    ("debug/config.txt",            "src/game/debug.cpp:422",  "debug config"),
    ("debug/roomInfo.dat",          "src/game/main.cpp:366",   "room jump menu"),
    ("d:\\bio4/room/savedata%02d.dat", "src/game/card.cpp:638", "SN host save (also :854, :2550)"),
    ("d:\\bio4/room/sysdata.dat",   "src/game/card.cpp:866",   "SN host save (also :2814)"),
    ("d:\\bio4/data/%s",            "src/game/dvd.cpp:868",    "SYS_SN_PC_READ prefix on every read"),
    ("x:/soft/room/event/evd/evt_bin_%s.xml", "src/game/event.cpp:365", "t_esp tool"),
    ("event/evd/r120s00.evd",       "src/game/event.cpp:325",  "dead strcmp"),
    ("event/evd/r100s40.evd",       "src/game/event.cpp:1345", "EvtDebugEvdName, never called"),
    ("r%03x.dat",                   "src/game/read.cpp:135",   "error message only"),
]
# Strings that look like paths but name entries INSIDE an archive (EvtMgr.SetBin tags such as
# "em/pl00/pl000a.bin" event.cpp:1503, "etc/core/dummy.bin" event.cpp:1509, "event/model/..",
# "event/r304/s00/cam/*.fcv" camera tracks inside the .evd), not disc files.
ARCHIVE_INTERNAL_RE = re.compile(r"^(em/[a-z0-9]+/|etc/core/|event/(?!evd/))")

# FileTbl indices read with DvdRead(fileNo) (resolved once at boot, never rewritten by SET_DRS_NAME):
BY_INDEX_READS = [
    (0x00, "src/game/snd.cpp:92"), (0x03, "src/game/read.cpp:199"), (0x59, "src/game/snd.cpp:130"),
    (0x60, "src/game/snd.cpp:102"), (0x61, "src/game/snd.cpp:2518"), (0x68, "src/game/snd.cpp:108"),
    (0x69, "src/game/snd.cpp:1690"), (0x6A, "src/game/snd.cpp:114"),
    ("Room_data_tbl[].rel_file", "src/game/roomdata.cpp:297"),
]

SCAN_DIRS = ("src/game", "src/Sscrn", "src/st1", "src/st2", "src/st3", "src/st4")
SCAN_RE = re.compile(r'"((?:etc|em|rel|Rel|st[0-9]|St[0-9]|SS|ss|Font|font|movie|Movie|bgm|op|Evd|evd|event|debug)/'
                     r'[A-Za-z0-9_./%-]+\.[a-z]{3})"')


# ----------------------------------------------------------------------------------------------
def read_text(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def parse_filetbl(dvd_cpp):
    """[(index, name)] from `FileTblEntry FileTbl[] = { {"name", 0}, ... };` (dvd.cpp:7)."""
    text = read_text(dvd_cpp)
    m = re.search(r"FileTblEntry\s+FileTbl\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not m:
        raise SystemExit("FileTbl[] not found in " + dvd_cpp)
    names = re.findall(r'\{\s*"([^"]*)"\s*,\s*-?\d+\s*\}', m.group(1))
    return list(enumerate(names))


def parse_retail(path):
    """{lower_path: dict(path, discs=set(), size)} from retail_filelist.txt."""
    files = {}
    for line in read_text(path).splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        disc, rel, size = line.rstrip("\n").split("\t")
        rel = rel.replace("\\", "/")
        e = files.setdefault(rel.lower(), dict(path=rel, discs=set(), size=int(size)))
        e["discs"].add(int(disc[-1]))
        if e["size"] != int(size):
            e["size_mismatch"] = True
    return files


class Resolver:
    def __init__(self, retail):
        self.retail = retail
        self.requested = {}   # lower retail path -> set(requesting names)
        self.rows = []        # (requested_name, retail_path or None, discs, size, source, kind, note)

    def add(self, name, source, kind, note=""):
        key = name.replace("\\", "/").lower()
        e = self.retail.get(key)
        if e:
            self.requested.setdefault(key, set()).add(name)
            self.rows.append((name, e["path"], e["discs"], e["size"], source, kind, note))
        else:
            self.rows.append((name, None, set(), 0, source, kind, note))
        return e

    def match_pattern(self, pat):
        rx = re.compile("^" + pat["regex"] + "$", re.I)
        hits = []
        for key, e in self.retail.items():
            m = rx.match(key)
            if m:
                g = m.groups()
                name = pat["code"](g if len(g) > 1 else g[0])
                hits.append(name)
                self.add(name, pat["src"], "pattern", pat["fmt"])
        return sorted(hits)


def das_reason(name):
    """Why a FileTbl name is absent on retail."""
    low = name.lower()
    base = low.rsplit("/", 1)[-1]
    if low in FILETBL_NOTES:
        return FILETBL_NOTES[low]
    if low.endswith(".drs") and low[:-4] + ".das" in FILETBL_NOTES:
        return FILETBL_NOTES[low[:-4] + ".das"]
    if low.startswith("em/") and low.endswith(".das"):
        return "template: SET_DRS_NAME (read.cpp:107) rewrites it to em/%s.drs before the read" % base[:-4]
    if low.startswith("rel/") and (base.startswith(("em", "wep", "pl"))):
        return "debug-only: the REL is embedded in em/%s.drs on every disc (config.yml:18); index only used as a flag (read.cpp:385)" % base[:-4]
    if low.startswith("rel/"):
        return "debug-only: not on retail (no Room_data_tbl row / debug stage)"
    return "absent on retail"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--repo", default=DEFAULT_REPO)
    ap.add_argument("--out", default=None, help="filenames.inc path (default xbox/port/dvd/filenames.inc)")
    ap.add_argument("--quiet", action="store_true", help="only print the summary counts")
    args = ap.parse_args()
    repo = args.repo
    out_inc = args.out or os.path.join(repo, "xbox", "port", "dvd", "filenames.inc")
    dvd_cpp = os.path.join(repo, "src", "game", "dvd.cpp")
    list_txt = os.path.join(HERE, "retail_filelist.txt")
    for p in (dvd_cpp, list_txt):
        if not os.path.exists(p):
            print("missing input:", p)
            return 2

    retail = parse_retail(list_txt)
    filetbl = parse_filetbl(dvd_cpp)
    R = Resolver(retail)
    out = [] if args.quiet else None

    def say(*a):
        s = " ".join(str(x) for x in a)
        if out is None:
            print(s)

    say("RE4 file table audit -- retail USA discs vs. the names the game requests")
    say("retail list: %d unique paths (%d entries)" % (len(retail), sum(len(e["discs"]) for e in retail.values())))

    # --- FileTbl ---------------------------------------------------------------------------------
    say("\n== FileTbl[] (src/game/dvd.cpp:7): %d entries" % len(filetbl))
    tbl_ok, tbl_case, tbl_das, tbl_debug, tbl_missing = [], [], [], [], []
    for idx, name in filetbl:
        e = R.add(name, "src/game/dvd.cpp:%d" % (8 + idx), "filetbl", "FileTbl[0x%02x]" % idx)
        if e:
            (tbl_case if e["path"] != name else tbl_ok).append((idx, name, e["path"]))
            continue
        low = name.lower()
        if low.startswith("em/") and low.endswith(".das"):
            drs = name[:-4] + ".drs"
            e2 = R.add(drs, "src/game/read.cpp:107", "filetbl-drs", "FileTbl[0x%02x] after SET_DRS_NAME" % idx)
            if e2:
                tbl_das.append((idx, name, e2["path"]))
                # alias: the .das spelling is what FileTblExistCheck (dvd.cpp:1266) converts at boot;
                # mapping it to the .drs file keeps entrynum valid before SET_DRS_NAME runs.
                R.rows.append((name, e2["path"], e2["discs"], e2["size"], "src/game/dvd.cpp:%d" % (8 + idx),
                               "filetbl-das-alias", "FileTbl[0x%02x] .das template -> .drs" % idx))
                continue
            tbl_missing.append((idx, name, das_reason(name)))
        elif low.startswith("rel/"):
            tbl_debug.append((idx, name, das_reason(name)))
        else:
            tbl_missing.append((idx, name, das_reason(name)))
    say("  resolved exactly:        %d" % len(tbl_ok))
    say("  resolved, case differs:  %d" % len(tbl_case))
    say("  .das -> .drs (rewritten): %d" % len(tbl_das))
    say("  debug-only (REL in .drs): %d" % len(tbl_debug))
    say("  MISSING on retail:       %d" % len(tbl_missing))
    for idx, name, why in tbl_missing:
        say("    [0x%02x] %-22s %s" % (idx, name, why))
    say("  by-index reads (DvdRead(fileNo), entrynum fixed at boot by FileTblExistCheck dvd.cpp:1261):")
    for idx, src in BY_INDEX_READS:
        if isinstance(idx, int):
            name = filetbl[idx][1]
            e = retail.get(name.lower())
            say("    [0x%02x] %-20s -> %-22s %s" % (idx, name, e["path"] if e else "MISSING", src))
        else:
            say("    %-28s -> rel/stN_M.rel (room RELs)   %s" % (idx, src))

    # --- static names ----------------------------------------------------------------------------
    say("\n== Static names (DvdReadN / FileExistCheck / DC.setData / Sofdec): %d" % (len(STATIC_NAMES) + len(LANG_NAMES) * len(LANGS)))
    st_missing, st_expected = [], []
    for name, src, note in STATIC_NAMES:
        if not R.add(name, src, "static", note):
            (st_expected if name.lower() in EXPECTED_ABSENT else st_missing).append((name, src, note))
    lang_present = {}
    for tmpl, src, note in LANG_NAMES:
        for lang in LANGS:
            name = tmpl.replace("___", lang)
            if R.add(name, src, "static-lang", note):
                lang_present.setdefault(lang, 0)
                lang_present[lang] += 1
            elif lang == "eng":
                st_missing.append((name, src, note))
    say("  language dirs present on retail: %s (of %s); non-eng SS/___ names are expected to be absent on the USA discs" % (
        ", ".join("%s=%d" % kv for kv in sorted(lang_present.items())), "/".join(LANGS)))
    for name, src, note in st_expected:
        say("  absent, expected: %-22s %-28s %s" % (name, src, EXPECTED_ABSENT[name.lower()]))
    for name, src, note in st_missing:
        say("  MISSING: %-26s %-30s %s" % (name, src, note))
    if not st_missing:
        say("  every other static name resolves (eng language set)")

    # --- dynamic patterns ------------------------------------------------------------------------
    say("\n== Dynamic name patterns (sprintf), expanded against the retail list:")
    pat_missing = []
    for pat in DYNAMIC_PATTERNS:
        hits = R.match_pattern(pat)
        say("  %-38s %-28s %4d files   %s" % (pat["fmt"], pat["src"], len(hits), pat["note"]))
        for exp in pat.get("expect", []):
            if exp.lower() not in retail:
                pat_missing.append((exp, pat["src"]))
                R.add(exp, pat["src"], "pattern-expected", pat["fmt"])
    for exp, src in pat_missing:
        say("  expansion absent on retail: %-16s %-26s %s" % (exp, src, EXPECTED_ABSENT.get(exp.lower(), "UNEXPECTED")))

    # --- debug-only ------------------------------------------------------------------------------
    say("\n== Debug-only names (never on a disc): %d" % len(DEBUG_ONLY))
    for name, src, note in DEBUG_ONLY:
        say("  %-42s %-28s %s" % (name, src, note))

    # --- cross-check: literal scan --------------------------------------------------------------
    known = {r[0].lower() for r in R.rows}
    known |= {n.lower() for n, _, _ in DEBUG_ONLY}
    scan_unknown = {}
    for d in SCAN_DIRS:
        root = os.path.join(repo, d)
        if not os.path.isdir(root):
            continue
        for dp, _, fns in os.walk(root):
            for fn in fns:
                if not fn.endswith((".cpp", ".h")):
                    continue
                for s in SCAN_RE.findall(read_text(os.path.join(dp, fn))):
                    low = s.lower()
                    if low.startswith("event/evd/"):
                        low = low[len("event/"):]          # EvtReadSub strstr("evd/") event.cpp:2295
                    low = low.replace("ss/___/", "ss/eng/").replace("omk_tx", "omk_t0")  # setLangExt3 / title.cpp:869
                    if "%" in s or low in known or ARCHIVE_INTERNAL_RE.match(low):
                        continue
                    if low in retail:
                        continue                            # literal that resolves but is not in the tables (fine)
                    scan_unknown.setdefault(s, set()).add(os.path.relpath(os.path.join(dp, fn), repo).replace("\\", "/"))
    say("\n== Literal-string scan of %s: %d unclassified name(s) absent on retail" % (", ".join(SCAN_DIRS), len(scan_unknown)))
    for s, where in sorted(scan_unknown.items()):
        say("  %-36s %s" % (s, ", ".join(sorted(where))))

    # --- summaries over the retail list ---------------------------------------------------------
    never = [e for k, e in sorted(retail.items()) if k not in R.requested]
    say("\n== Retail files never requested by any name or pattern: %d" % len(never))
    for e in never:
        say("  %-32s disc%s %d" % (e["path"], "+".join(str(d) for d in sorted(e["discs"])), e["size"]))

    case_rows = sorted({(r[0], r[1]) for r in R.rows if r[1] and r[0] != r[1] and r[5] != "filetbl-das-alias"})
    dir_mismatch = {}
    for req, real in case_rows:
        a, b = req.rsplit("/", 1)[0] if "/" in req else "", real.rsplit("/", 1)[0] if "/" in real else ""
        dir_mismatch.setdefault((a, b), 0)
        dir_mismatch[(a, b)] += 1
    say("\n== Name-case mismatches (code spelling -> disc spelling): %d names; GC DVDConvertPathToEntrynum is case-insensitive (src/lib/dvdfs.c:50)" % len(case_rows))
    for (a, b), n in sorted(dir_mismatch.items()):
        if a != b:
            say("  %-14s -> %-14s %d" % (a + "/", b + "/", n))
    for req, real in case_rows:
        if req.rsplit("/", 1)[-1] != real.rsplit("/", 1)[-1]:
            say("  file-name case: %s -> %s" % (req, real))

    d2 = [e for k, e in sorted(retail.items()) if e["discs"] == {2}]
    d1 = [e for k, e in sorted(retail.items()) if e["discs"] == {1}]
    say("\n== Files only on disc 2: %d  (only on disc 1: %d, on both: %d)" % (len(d2), len(d1), len(retail) - len(d1) - len(d2)))
    for e in d2:
        say("  %-32s %d" % (e["path"], e["size"]))
    bad = [e["path"] for e in retail.values() if e.get("size_mismatch")]
    if bad:
        say("  size differs between discs: %s" % ", ".join(bad))

    # --- filenames.inc ---------------------------------------------------------------------------
    write_inc(out_inc, R, filetbl)
    resolved = {r[0].lower() for r in R.rows if r[1]}
    unresolved = {r[0].lower() for r in R.rows if not r[1] and r[5] != "static-lang"} - resolved
    print("\n== Summary: %d requested names resolve to a retail path (%d of them .das spellings aliased to .drs), "
          "%d do not (FileTbl debug-only rel/ %d, FileTbl missing %d, static absent-as-expected %d, static MISSING %d, "
          "pattern expansions absent %d, scan unclassified %d), %d retail files never requested, %d disc-2-only "
          "files, %d name-case mismatches. Wrote %s" % (
              len(resolved), len(tbl_das), len(unresolved), len(tbl_debug), len(tbl_missing), len(st_expected),
              len(st_missing), len(pat_missing), len(scan_unknown), len(never), len(d2), len(case_rows),
              os.path.relpath(out_inc, repo).replace("\\", "/")))
    return 0


def write_inc(path, R, filetbl):
    """xbox/port/dvd/filenames.inc: X-macro table, one line per requested name."""
    lines = [
        "/* Generated by xbox/tools/filelist_check.py from src/game/dvd.cpp FileTbl[] and",
        " * xbox/tools/retail_filelist.txt -- DO NOT EDIT.",
        " *",
        " * RE4_FILE(requested_name, retail_path, disc_mask, size, kind)",
        " *   requested_name  the string the game passes to DvdRead/DvdReadN (code spelling, may differ in case)",
        " *   retail_path     the exact path under files/ on the retail USA disc",
        " *   disc_mask       1 = disc 1, 2 = disc 2, 3 = on both",
        " *   kind            filetbl | filetbl_drs (name after SET_DRS_NAME, read.cpp:107) |",
        " *                   filetbl_das_alias (the .das spelling, mapped to the same .drs file so the boot-time",
        " *                   FileTblExistCheck dvd.cpp:1266 gets a valid entry) | static | static_lang |",
        " *                   pattern (sprintf expansion)",
        " * RE4_FILE_MISSING(requested_name, reason)   names the game can build that no retail disc has",
        " * RE4_FILETBL_INDEX(index, name)              FileTbl[] order, for DvdRead(fileNo) callers",
        " */",
        "#ifndef RE4_FILE",
        "#define RE4_FILE(req, path, disc, size, kind)",
        "#endif",
        "#ifndef RE4_FILE_MISSING",
        "#define RE4_FILE_MISSING(req, reason)",
        "#endif",
        "#ifndef RE4_FILETBL_INDEX",
        "#define RE4_FILETBL_INDEX(idx, name)",
        "#endif",
        "",
    ]
    for idx, name in filetbl:
        lines.append('RE4_FILETBL_INDEX(0x%02x, "%s")' % (idx, name))
    lines.append("")
    seen = {}
    for name, real, discs, size, src, kind, note in R.rows:
        if not real or name.lower() in seen:
            continue
        seen[name.lower()] = 1
        mask = sum(1 << (d - 1) for d in discs)
        lines.append('RE4_FILE("%s", "%s", %d, %d, %s) /* %s */' % (name, real, mask, size, kind.replace("-", "_"), src))
    lines.append("")
    for name, real, discs, size, src, kind, note in R.rows:
        if real or kind == "static-lang" or name.lower() in seen:
            continue
        seen[name.lower()] = 1
        if kind.startswith("filetbl"):
            reason = das_reason(name)
        else:
            reason = EXPECTED_ABSENT.get(name.lower(), "not on retail USA discs; " + note)
        lines.append('RE4_FILE_MISSING("%s", "%s") /* %s */' % (name.replace("\\", "\\\\"), reason.replace('"', "'"), src))
    lines += ["", "#undef RE4_FILE", "#undef RE4_FILE_MISSING", "#undef RE4_FILETBL_INDEX", ""]
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(lines))


if __name__ == "__main__":
    sys.exit(main())
