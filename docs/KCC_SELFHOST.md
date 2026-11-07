# KCC self-hosting path

The normal `/bin/kcc` compiler remains the modular build. The build also
generates and installs:

- `/bin/kcc-selfhost` — the same compiler/runtime assembled into one
  translation unit, with backend-local static symbols namespaced.
- `/data/kcc/kcc_selfhost.c` — the complete source used for the in-guest
  self-hosting pass.
- `/etc/kcc/include/kcc.h`, target/backend headers, and the nested
  `/etc/kcc/include/kccrt/` runtime headers.

The KCC reader now permits safe nested include paths while rejecting absolute
paths, empty components, `.`, `..`, backslashes, and non-source path
characters. Include search order is the KCC runtime headers first, then the
general KIRA C headers.

From the KIRA shell, compile the installed source with the running compiler:

```sh
kcc kcc_selfhost.c kcc-stage3.elf
```

The resulting ELF is a stage-3 compiler produced by KCC itself. This source
build includes both x86-64 and AArch64 compiler backends. Runtime execution and
a successful stage-3 rebuild still need to be demonstrated on the guest; the
build now supplies the code and all required source/header inputs.
