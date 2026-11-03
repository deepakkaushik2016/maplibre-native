#!/usr/bin/env python3
"""Assemble each line program's shader source the way the engine does and validate it.

GL     : ShaderProgramGL::create   -> "#version 300 es\n" + defines + HAS_UNIFORM defines + Prelude + source
Vulkan : vulkan::ShaderProgram     -> "#version 450\n" + preamble(defines + "\n" + Prelude) + (lineShadePrelude + source)
WebGPU : webgpu::ShaderGroup       -> Prelude + "\n" + preprocessWGSL(source, defines)   (engine preprocessor via wgslpp)
Static compile/validation only - it proves the sources compile, not that they render.

Usage: scripts/shadercheck/check_line_shaders.py [repo] [rev|WORKTREE] [outdir]
       (defaults: the repo containing this script, WORKTREE, a temp dir)
Env:   GLSLANG  path to glslang / glslangValidator (default: from PATH)
       NAGA     path to naga-cli (default: from PATH; `cargo install naga-cli`)
       CXX      C++ compiler for the wgslpp helper (default: clang++)
       MUTATE=1 negative control: break the draped-flag reads, every vertex stage must fail
"""
import itertools, os, re, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
repo = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.dirname(HERE))
rev = sys.argv[2] if len(sys.argv) > 2 else "WORKTREE"
out = sys.argv[3] if len(sys.argv) > 3 else tempfile.mkdtemp(prefix="shadercheck-")
GLSLANG = os.environ.get("GLSLANG") or shutil.which("glslang") or shutil.which("glslangValidator")
NAGA = os.environ.get("NAGA") or shutil.which("naga")
if not GLSLANG or not NAGA:
    sys.exit("need glslang and naga (set GLSLANG / NAGA)")
os.makedirs(out, exist_ok=True)

# Build the WGSL preprocessor helper from the engine's own preprocessor source.
WGSLPP = os.path.join(out, "wgslpp")
subprocess.run([os.environ.get("CXX", "clang++"), "-std=c++20", "-O1", "-I", os.path.join(repo, "include"),
                os.path.join(HERE, "wgslpp.cpp"), os.path.join(repo, "src/mln/shaders/webgpu/wgsl_preprocessor.cpp"),
                "-o", WGSLPP], check=True)

MUTATE = os.environ.get("MUTATE") == "1"  # negative control: break the draped-flag reads

def read(path):
    if rev == "WORKTREE":
        text = open(os.path.join(repo, path)).read()
        return text.replace("pad1 != 0.0", "padX != 0.0") if MUTATE else text
    return subprocess.run(["git", "-C", repo, "show", f"{rev}:{path}"], capture_output=True, text=True, check=True).stdout

def raw(text, name):
    m = re.search(r"static constexpr (?:auto|const char\*) " + name + r' = R"\(', text)
    if not m:
        return None
    end = text.index(')"', m.end())
    return text[m.end():end]

def section(text, builtin, backend):
    start = text.index(f"struct ShaderSource<BuiltIn::{builtin}, gfx::Backend::Type::{backend}>")
    nxt = text.find("struct ShaderSource<", start + 10)
    return text[start: nxt if nxt > 0 else len(text)]

PROGRAMS = [("LineShader", "line"), ("LineGradientShader", "line_gradient"), ("LinePatternShader", "line_pattern"),
            ("LineSDFShader", "line_sdf"), ("LineGradientSDFShader", "line_gradient_sdf")]
DPR = "#define DEVICE_PIXEL_RATIO 1.000000\n"

def uniforms(*srcs):
    return sorted(set(itertools.chain.from_iterable(re.findall(r"HAS_UNIFORM_u_\w+", s) for s in srcs)))

def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode, (r.stdout + r.stderr).strip()

failures = 0
def report(label, rc, msg):
    global failures
    status = "OK  " if rc == 0 else "FAIL"
    failures += rc != 0
    print(f"{status} {label}")
    if rc != 0:
        print("     " + msg.replace("\n", "\n     ")[:3000])

gl_prelude = read("include/mln/shaders/gl/prelude.hpp")
vk_line = read("include/mln/shaders/vulkan/line.hpp")
vk_common = read("include/mln/shaders/vulkan/common.hpp")
vk_common_sec = section(vk_common, "Prelude", "Vulkan")
wg_line = read("include/mln/shaders/webgpu/line.hpp")
wg_prelude = raw(section(read("include/mln/shaders/webgpu/common.hpp"), "Prelude", "WebGPU"), "prelude")
vk_shade_prelude = raw(vk_line.replace("constexpr auto lineShadePrelude = R\"(", "static constexpr auto lineShadePrelude = R\"("), "lineShadePrelude")

for builtin, base in PROGRAMS:
    # ---- GL (GLSL ES 3.00), vertex + fragment linked
    hdr = read(f"include/mln/shaders/gl/{base}.hpp")
    v, f = raw(hdr, "vertex"), raw(hdr, "fragment")
    for perm, defs in (("none", []), ("all", uniforms(v, f))):
        extra = "".join(f"#define {d}\n" for d in defs)
        vs = "#version 300 es\n" + DPR + extra + raw(gl_prelude, "vertex") + v
        fs = "#version 300 es\n" + DPR + extra + raw(gl_prelude, "fragment") + f
        vp, fp = f"{out}/gl-{base}-{perm}.vert", f"{out}/gl-{base}-{perm}.frag"
        open(vp, "w").write(vs); open(fp, "w").write(fs)
        rc, msg = run([GLSLANG, "-l", vp, fp])
        report(f"GL     {builtin:22} uniforms={perm}", rc, msg)

    # ---- Vulkan (GLSL 450 -> SPIR-V, Vulkan rules), with/without surface transform
    sec = section(vk_line, builtin, "Vulkan")
    v, f = raw(sec, "vertex"), raw(sec, "fragment")
    for perm, defs in (("none", []), ("all", uniforms(v, f))):
        for surf in ("", "#define USE_SURFACE_TRANSFORM"):
            defstr = DPR + "\n\n" + "".join(f"#define {d} \n" for d in defs) + surf
            for stage, src, pre in (("vert", v, raw(vk_common_sec, "vertex")), ("frag", f, raw(vk_common_sec, "fragment"))):
                text = "#version 450\n" + defstr + "\n" + pre + vk_shade_prelude + src
                p = f"{out}/vk-{base}-{perm}{'-st' if surf else ''}.{stage}"
                open(p, "w").write(text)
                rc, msg = run([GLSLANG, "-V", "--target-env", "vulkan1.0", "-o", os.devnull, p])
                report(f"Vulkan {builtin:22} {stage} uniforms={perm} surfaceTransform={'on' if surf else 'off'}", rc, msg)

    # ---- WebGPU (WGSL), each stage is its own module
    sec = section(wg_line, builtin, "WebGPU")
    v, f = raw(sec, "vertex"), raw(sec, "fragment")
    for perm, defs in (("none", []), ("all", uniforms(v, f))):
        for stage, src in (("vert", v), ("frag", f)):
            pp = subprocess.run([WGSLPP, "DEVICE_PIXEL_RATIO", *defs], input=src, capture_output=True, text=True, check=True).stdout
            p = f"{out}/wgpu-{base}-{perm}-{stage}.wgsl"
            open(p, "w").write(wg_prelude + "\n" + pp)
            rc, msg = run([NAGA, p])
            report(f"WebGPU {builtin:22} {stage} uniforms={perm}", rc, msg)

print(f"\n{failures} failure(s)")
sys.exit(1 if failures else 0)
