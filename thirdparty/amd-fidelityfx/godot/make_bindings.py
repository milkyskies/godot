#!/usr/bin/env python3
"""Godot (hacco #1441): the binding table of every FSR 3 frame generation pass, read from its compiled GLSL.

The SDK's own backends read a pass's bindings (slot and name) from reflection data its shader compiler writes beside
each permutation; Godot keeps no names, so this writes them out once with glslang's reflection:

    python3 thirdparty/amd-fidelityfx/godot/make_bindings.py   (from the Godot source root; needs glslangValidator)

writes servers/rendering/renderer_rd/effects/frame_generation_bindings.inc. Run it again after updating the SDK.
"""

import os
import re
import subprocess
import sys
import tempfile

PASSES = [
    # (effect, pass enum, shader file, class name)
    ("FI", "FFX_FRAMEINTERPOLATION_PASS_RECONSTRUCT_AND_DILATE", "ffx_frameinterpolation_reconstruct_and_dilate_pass"),
    ("FI", "FFX_FRAMEINTERPOLATION_PASS_SETUP", "ffx_frameinterpolation_setup_pass"),
    ("FI", "FFX_FRAMEINTERPOLATION_PASS_RECONSTRUCT_PREV_DEPTH", "ffx_frameinterpolation_reconstruct_previous_depth_pass"),
    ("FI", "FFX_FRAMEINTERPOLATION_PASS_GAME_MOTION_VECTOR_FIELD", "ffx_frameinterpolation_game_motion_vector_field_pass"),
    ("FI", "FFX_FRAMEINTERPOLATION_PASS_OPTICAL_FLOW_VECTOR_FIELD", "ffx_frameinterpolation_optical_flow_vector_field_pass"),
    ("FI", "FFX_FRAMEINTERPOLATION_PASS_DISOCCLUSION_MASK", "ffx_frameinterpolation_disocclusion_mask_pass"),
    ("FI", "FFX_FRAMEINTERPOLATION_PASS_INTERPOLATION", "ffx_frameinterpolation_pass"),
    ("FI", "FFX_FRAMEINTERPOLATION_PASS_INPAINTING_PYRAMID", "ffx_frameinterpolation_compute_inpainting_pyramid_pass"),
    ("FI", "FFX_FRAMEINTERPOLATION_PASS_INPAINTING", "ffx_frameinterpolation_inpainting_pass"),
    ("FI", "FFX_FRAMEINTERPOLATION_PASS_GAME_VECTOR_FIELD_INPAINTING_PYRAMID", "ffx_frameinterpolation_compute_game_vector_field_inpainting_pyramid_pass"),
    ("FI", "FFX_FRAMEINTERPOLATION_PASS_DEBUG_VIEW", "ffx_frameinterpolation_debug_view_pass"),
    ("OF", "FFX_OPTICALFLOW_PASS_PREPARE_LUMA", "ffx_opticalflow_prepare_luma_pass"),
    ("OF", "FFX_OPTICALFLOW_PASS_GENERATE_OPTICAL_FLOW_INPUT_PYRAMID", "ffx_opticalflow_compute_luminance_pyramid_pass"),
    ("OF", "FFX_OPTICALFLOW_PASS_GENERATE_SCD_HISTOGRAM", "ffx_opticalflow_generate_scd_histogram_pass"),
    ("OF", "FFX_OPTICALFLOW_PASS_COMPUTE_SCD_DIVERGENCE", "ffx_opticalflow_compute_scd_divergence_pass"),
    ("OF", "FFX_OPTICALFLOW_PASS_COMPUTE_OPTICAL_FLOW_ADVANCED_V5", "ffx_opticalflow_compute_optical_flow_advanced_pass_v5"),
    ("OF", "FFX_OPTICALFLOW_PASS_FILTER_OPTICAL_FLOW_V5", "ffx_opticalflow_filter_optical_flow_pass_v5"),
    ("OF", "FFX_OPTICALFLOW_PASS_SCALE_OPTICAL_FLOW_ADVANCED_V5", "ffx_opticalflow_scale_optical_flow_advanced_pass_v5"),
]

DEFINES = """#version 450
#extension GL_GOOGLE_include_directive : require
#define FFX_GPU 1
#define FFX_GLSL 1
#define FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS 1
#define FFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS 0
#define FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH 1
"""


SAMPLED = {0x8B5E: "TEXTURE2D", 0x8DD2: "UTEXTURE2D", 0x8DCA: "ITEXTURE2D"}


def is_image(type_code):
    return 0x904C <= type_code <= 0x906C


def reflect(path):
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "pass.comp")
        with open(src, "w") as f:
            f.write(DEFINES + '#include "%s"\n' % path)
        pre = subprocess.run(["glslangValidator", "-E", "-I.", src], capture_output=True, text=True, check=True).stdout
        out = subprocess.run(
            ["glslangValidator", "--target-env", "vulkan1.2", "-I.", "-S", "comp", "-q", src, "-o", os.path.join(tmp, "o.spv")],
            capture_output=True, text=True,
        )
        if out.returncode != 0:
            sys.exit(out.stdout + out.stderr)
    instances = {}  # block type name -> (instance name, is storage buffer)
    for m in re.finditer(r"\b(uniform|buffer)\s+(\w+)\s*\{[^}]*\}\s*(\w+)\s*;", pre):
        instances[m.group(2)] = (m.group(3), m.group(1) == "buffer")
    section = None
    res = {"srv_tex": [], "uav_tex": [], "srv_buf": [], "uav_buf": [], "cb": []}
    for line in out.stdout.splitlines():
        if line.endswith("reflection:"):
            section = line
            continue
        m = re.match(r"(\w+): offset (-?\d+), type ([0-9a-f]+),.* binding (-?\d+)", line)
        if not m:
            continue
        name, type_code, binding = m.group(1), int(m.group(3), 16), int(m.group(4))
        if section == "Uniform reflection:" and binding >= 0:
            if name.startswith("s_"):
                continue
            if is_image(type_code):
                res["uav_tex"].append((binding, name))
            else:
                kind = SAMPLED.get(type_code)
                if kind is None:
                    sys.exit("%s: %s has a sampled type 0x%x this does not know" % (path, name, type_code))
                res["srv_tex"].append((binding, name, kind))
        elif section == "Uniform block reflection:":
            inst, ssbo = instances[name]
            if ssbo:
                res["srv_buf" if inst.startswith("r_") else "uav_buf"].append((binding, inst))
            else:
                res["cb"].append((binding, inst))
    for k in res:
        res[k].sort()
    used = {item[0] for k in res for item in res[k]}
    # Every binding the SPIR-V declares, used or not: Godot's reflection asks for each in the uniform set, so the ones
    # the pass never reads get a placeholder of their kind.
    declared = []
    for m in re.finditer(r"layout\s*\(([^)]*)\)\s*((?:\w+\s+)*?)(uniform|buffer)\s+(\w+)\s*(\{|\w+\s*;)", pre):
        quals, binding = m.group(1), re.search(r"binding\s*=\s*(\d+)", m.group(1))
        if not binding:
            continue
        binding = int(binding.group(1))
        if binding in used or binding == 1000:
            continue
        kind = m.group(4) if m.group(5) != "{" else ("ssbo" if m.group(3) == "buffer" else "ubo")
        declared.append((binding, kind))
    res["unused"] = sorted(set(declared))
    return res


def main():
    base = "thirdparty/amd-fidelityfx/FidelityFX/gpu/passes/"
    lines = [
        "/* Generated by thirdparty/amd-fidelityfx/godot/make_bindings.py (hacco #1441): each FSR 3 frame generation pass's",
        "   bindings, in the order the SDK expects them. Do not edit. */",
        "",
    ]
    for effect, enum, shader in PASSES:
        r = reflect(base + shader + ".glsl")
        lines.append("{ FrameGenerationTables::EFFECT_%s, %s, {" % (effect, enum))
        lines.append("\t\t{ %s }," % ", ".join(
            '{ %d, L"%s", FrameGenerationTables::PLACEHOLDER_%s }' % b for b in r["srv_tex"]))
        for kind in ("uav_tex", "srv_buf", "uav_buf", "cb"):
            items = ", ".join('{ %d, L"%s" }' % b for b in r[kind])
            lines.append("\t\t{ %s }," % items)
        unused = ", ".join("{ %d, FrameGenerationTables::PLACEHOLDER_%s }" % (b, k.upper()) for b, k in r["unused"])
        lines.append("\t\t{ %s }," % unused)
        lines.append("} },")
    out = "servers/rendering/renderer_rd/effects/frame_generation_bindings.inc"
    with open(out, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote", out)


main()
