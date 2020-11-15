Tuning optimization compiler flags for [vcxsrv](https://sourceforge.net/projects/vcxsrv/). Hopefully the performance and latency can be improved.
- Enabling AVX2 and LTO (`/arch:AVX2 /O2 /Oy /GL`) improves `x11perf` results by a few percent.
