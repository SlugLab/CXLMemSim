# CXLMemSim Homebrew Tap

This tap currently provides a macOS Apple Silicon formula for bpftime v0.2.0.

The formula builds bpftime in its non-Linux mode:

```sh
BPFTIME_BUILD_WITH_LIBBPF=OFF
BPFTIME_BUILD_KERNEL_BPF=OFF
BPFTIME_LLVM_JIT=OFF
BPFTIME_UBPF_JIT=ON
```

That gives us the bpftime CLI/runtime first, without relying on Linux kernel
eBPF pieces. The CXLMemSim LLVM pass / sanitizer-style instrumentation can then
link against or communicate with this installed runtime.

## Install Into A Local Tap

```sh
./install-local.sh
```

This creates or reuses the local tap `cxlmemsim/bpftime`, copies the formula
there, and runs:

```sh
brew install cxlmemsim/bpftime/bpftime
```

## Manual Local Tap

```sh
brew tap-new cxlmemsim/bpftime
cp /Users/yiweiyang/Documents/Lanxin/CXLMemSim/packaging/homebrew-tap/Formula/bpftime.rb \
  "$(brew --repository cxlmemsim/bpftime)/Formula/bpftime.rb"
brew install cxlmemsim/bpftime/bpftime
```

After installation:

```sh
bpftime --help
bpftimetool --help
```
