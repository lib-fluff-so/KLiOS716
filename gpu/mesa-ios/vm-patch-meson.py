import pathlib

p = pathlib.Path("meson.build")
s = p.read_text()
old = """else
  # We should prevent libdrm from being available when the target doesn't have it to avoid transitive
  # dependencies (such as vk-runtime) linking to it
  dep_libdrm = null_dep
endif"""
new = """elif host_machine.system() == 'darwin'
  # SweetMali: iOS has no KMS, but gallium panfrost still needs the libdrm API.
  # gpu/mali-sweet ships a libdrm stand-in that talks to the SweetMali kext.
  dep_libdrm = dependency('libdrm', version : '>=' + _drm_ver, required : true)
else
  # We should prevent libdrm from being available when the target doesn't have it to avoid transitive
  # dependencies (such as vk-runtime) linking to it
  dep_libdrm = null_dep
endif"""
if old in s:
    p.write_text(s.replace(old, new))
    print("root meson patched")
elif "SweetMali: iOS has no KMS" in s:
    print("root meson already patched")
else:
    raise SystemExit("root meson pattern not found")

q = pathlib.Path("src/gallium/targets/osmesa/meson.build")
t = q.read_text()
old2 = "  osmesa_sweet_deps += [driver_panfrost, dep_libdrm]"
new2 = "  osmesa_sweet_deps += [driver_panfrost, dep_libdrm, idep_xmlconfig]"
if old2 in t:
    q.write_text(t.replace(old2, new2))
    print("osmesa meson patched")
elif new2 in t:
    print("osmesa meson already patched")
else:
    raise SystemExit("osmesa meson pattern not found")
