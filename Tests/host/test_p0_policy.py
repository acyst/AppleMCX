#!/usr/bin/env python3
import pathlib
import plistlib
import re


ROOT = pathlib.Path(__file__).resolve().parents[2]


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


plist_paths = [
    ROOT / "AppleMCX.kext/Contents/Info.plist",
    ROOT / "AppleMCX.kext/Contents/Info.plist.tmpl",
]
plists = [plistlib.loads(path.read_bytes()) for path in plist_paths]
assert plists[0] == plists[1]
roce = plists[0]["IOKitPersonalities"]["MlxRoCE"]
assert roce["IOProviderClass"] == "MlxPCIDriver"
assert roce["IOPropertyMatch"] == {"mlx_rdma": True}

core = read("Sources/core/MlxPCIDriver.cpp")
assert re.search(
    r"#ifndef APPLEMCX_ENABLE_UNSAFE_ROCE\s+"
    r"#define APPLEMCX_ENABLE_UNSAFE_ROCE 0",
    core,
)
assert "if (rocePublicationAllowed())" in core
assert "return false;\n#endif" in core

roce_source = read("Sources/ib/MlxRoCE.cpp")
assert "if (!fCore->rocePublicationAllowed())" in roce_source
assert "cleanupResources();" in roce_source
assert "fGID->setGID" in roce_source and "!=\n        kIOReturnSuccess" in roce_source

for path in [
    "Sources/netif/MlxEthernet.cpp",
    "usermode/libmlx/libmlx.c",
]:
    source = read(path)
    assert "fm_ce_se = MLX_WQE_CTRL_CQ_UPDATE" in source
    assert not re.search(r"fm_ce_se\s*=\s*0x0?2\b", source)
