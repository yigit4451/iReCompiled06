#!/usr/bin/env python3
"""
Generates valid fallback PPC & Xenos shader-cache translation units when
copyrighted Xbox 360 retail assets (MarathonRecompLib/private/default.xex,
shader.arc, shader_lt.arc) are absent (for example in public CI builds).

When private/default.xex is present, MarathonRecompLib/CMakeLists.txt runs
the full XenonRecomp + u8extract + XenosRecomp pipeline instead of this script.
"""

from __future__ import annotations

import argparse
import pathlib
import re


SOURCE_EXTENSIONS = {".h", ".hpp", ".c", ".cpp", ".mm", ".inl"}

SUB_RE = re.compile(r"\b(sub_([0-9A-Fa-f]{8}))\b")
IMP_SUB_RE = re.compile(r"\b__imp__(sub_([0-9A-Fa-f]{8}))\b")
IMP_RE = re.compile(r"\b(__imp__[A-Za-z0-9_]+)\b")
STRONG_DEF_RE = re.compile(
    r"(?:GUEST_FUNCTION_HOOK|GUEST_FUNCTION_STUB|PPC_FUNC|PPC_FUNC_IMPL)\s*\(\s*([A-Za-z0-9_]+)\b"
)
# Exclude forward declarations ending with ');'
FORWARD_DECL_RE = re.compile(
    r"PPC_FUNC(?:_IMPL)?\s*\(\s*([A-Za-z0-9_]+)\s*\)\s*;"
)


def write_if_changed(path: pathlib.Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        try:
            if path.read_text(encoding="utf-8") == content:
                return
        except OSError:
            pass
    path.write_text(content, encoding="utf-8")


def scan_marathon_sources(scan_root: pathlib.Path) -> tuple[dict[str, int], set[str], set[str]]:
    subs: dict[str, int] = {}
    imps: set[str] = set()
    defined_syms: set[str] = set()

    for file_path in sorted(scan_root.rglob("*")):
        if not file_path.is_file() or file_path.suffix.lower() not in SOURCE_EXTENSIONS:
            continue
        try:
            text = file_path.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue

        forward_decls = set(FORWARD_DECL_RE.findall(text))
        stripped_text = FORWARD_DECL_RE.sub("", text)

        for sym in STRONG_DEF_RE.findall(stripped_text):
            defined_syms.add(sym)

        for full_name, hex_addr in SUB_RE.findall(text):
            subs[full_name] = int(hex_addr, 16)

        for sub_name, hex_addr in IMP_SUB_RE.findall(text):
            subs[sub_name] = int(hex_addr, 16)

        for imp_name in IMP_RE.findall(text):
            if not imp_name.startswith("__imp__sub_"):
                imps.add(imp_name)

        # Ensure forward-only declarations aren't mistaken for definitions
        _ = forward_decls

    return subs, imps, defined_syms


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate fallback MarathonRecompLib sources.")
    parser.add_argument("--scan-dir", required=True, type=pathlib.Path)
    parser.add_argument("--ppc-context-in", required=True, type=pathlib.Path)
    parser.add_argument("--ppc-out-dir", required=True, type=pathlib.Path)
    parser.add_argument("--shader-out-file", required=True, type=pathlib.Path)
    args = parser.parse_args()

    subs, imps, defined_syms = scan_marathon_sources(args.scan_dir)

    ppc_config = """#pragma once
#ifndef PPC_CONFIG_H_INCLUDED
#define PPC_CONFIG_H_INCLUDED

#define PPC_IMAGE_BASE 0x82000000ull
#define PPC_IMAGE_SIZE 0x1200000ull
#define PPC_CODE_BASE 0x82000000ull
#define PPC_CODE_SIZE 0x1000000ull

#endif
"""
    write_if_changed(args.ppc_out_dir / "ppc_config.h", ppc_config)

    ppc_context_body = args.ppc_context_in.read_text(encoding="utf-8")
    ppc_context = f'#pragma once\n#include "ppc_config.h"\n\n{ppc_context_body}\n'
    write_if_changed(args.ppc_out_dir / "ppc_context.h", ppc_context)

    sorted_subs = sorted(subs.items(), key=lambda kv: kv[1])
    sorted_imps = sorted(imps)

    shared_lines = [
        "#pragma once",
        "",
        '#include "ppc_config.h"',
        '#include "ppc_context.h"',
        "",
    ]
    for sub_name, _ in sorted_subs:
        shared_lines.append(f"PPC_EXTERN_FUNC({sub_name});")
    for imp_name in sorted_imps:
        shared_lines.append(f"PPC_EXTERN_FUNC({imp_name});")
    shared_lines.append("")
    write_if_changed(args.ppc_out_dir / "ppc_recomp_shared.h", "\n".join(shared_lines))

    mapping_lines = [
        '#include "ppc_recomp_shared.h"',
        "",
        "PPCFuncMapping PPCFuncMappings[] = {",
    ]
    for sub_name, addr in sorted_subs:
        mapping_lines.append(f"    {{ 0x{addr:X}ull, {sub_name} }},")
    mapping_lines.append("    { 0, nullptr }")
    mapping_lines.append("};")
    mapping_lines.append("")
    write_if_changed(args.ppc_out_dir / "ppc_func_mapping.cpp", "\n".join(mapping_lines))

    recomp_lines = [
        '#include "ppc_recomp_shared.h"',
        "",
    ]
    for sub_name, _ in sorted_subs:
        recomp_lines.append(f"PPC_FUNC_IMPL(__imp__{sub_name}) {{")
        recomp_lines.append("    PPC_FUNC_PROLOGUE();")
        recomp_lines.append("    (void)ctx;")
        recomp_lines.append("    (void)base;")
        recomp_lines.append("}")
        recomp_lines.append("")
        # Always provide weak fallback wrappers. The source scanner cannot
        # reliably distinguish every real definition form; a strong project
        # definition overrides this weak one, while omitted symbols otherwise
        # leave PPCFuncMappings with unresolved references at link time.
        recomp_lines.append(f"PPC_WEAK_FUNC({sub_name}) {{")
        recomp_lines.append(f"    __imp__{sub_name}(ctx, base);")
        recomp_lines.append("}")
        recomp_lines.append("")

    for imp_name in sorted_imps:
        # As above, weak fallback definitions are safe when a strong
        # implementation exists and prevent unresolved import references.
        recomp_lines.append(f"PPC_WEAK_FUNC({imp_name}) {{")
        recomp_lines.append("    (void)ctx;")
        recomp_lines.append("    (void)base;")
        recomp_lines.append("}")
        recomp_lines.append("")

    write_if_changed(args.ppc_out_dir / "ppc_recomp.0.cpp", "\n".join(recomp_lines))

    # Valid empty Zstandard frame (magic 0xFD2FB528 + empty single-segment frame header)
    shader_cache_cpp = """#include <cstddef>
#include <cstdint>
#include "shader_cache.h"

ShaderCacheEntry g_shaderCacheEntries[1] = {};
const size_t g_shaderCacheEntryCount = 0;

const uint8_t g_compressedDxilCache[] = { 0x28, 0xB5, 0x2F, 0xFD, 0x20, 0x00, 0x01, 0x00, 0x00 };
const size_t g_dxilCacheCompressedSize = sizeof(g_compressedDxilCache);
const size_t g_dxilCacheDecompressedSize = 0;

const uint8_t g_compressedAirCache[] = { 0x28, 0xB5, 0x2F, 0xFD, 0x20, 0x00, 0x01, 0x00, 0x00 };
const size_t g_airCacheCompressedSize = sizeof(g_compressedAirCache);
const size_t g_airCacheDecompressedSize = 0;

const uint8_t g_compressedSpirvCache[] = { 0x28, 0xB5, 0x2F, 0xFD, 0x20, 0x00, 0x01, 0x00, 0x00 };
const size_t g_spirvCacheCompressedSize = sizeof(g_compressedSpirvCache);
const size_t g_spirvCacheDecompressedSize = 0;
"""
    write_if_changed(args.shader_out_file, shader_cache_cpp)


if __name__ == "__main__":
    main()
