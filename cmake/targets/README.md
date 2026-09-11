# Target profiles

Target JSON files under `config/targets` record deployment constraints and
runtime expectations. CMake presets select the corresponding build options.
The Windows x86 profile is the only profile with ZKTeco support because the
PullSDK dependency is a 32-bit Windows DLL.
