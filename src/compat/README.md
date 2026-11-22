# compat

Headers that let the decomp's sources compile with a host compiler: standard
headers the Switch toolchain provided that the host lacks, and small fixes for
differences between the two. This directory comes first on the include path.

Keep it minimal. A header belongs here only if the decomp includes it by a name
the host does not have. Anything that implements behaviour belongs in a platform
layer directory instead (see `docs/ARCHITECTURE.md`).
