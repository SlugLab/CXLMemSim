# frozen_string_literal: true

class Bpftime < Formula
  desc "Userspace eBPF runtime and tooling"
  homepage "https://github.com/eunomia-bpf/bpftime"
  url "https://github.com/eunomia-bpf/bpftime.git",
      tag:      "v0.2.0",
      revision: "20acd8835241805b08bf256d0cf8963fd1cfbcac"
  license "MIT"
  head "https://github.com/eunomia-bpf/bpftime.git", branch: "master"

  depends_on "cmake" => :build
  depends_on "pkgconf" => :build
  depends_on "boost"
  depends_on "ncurses"

  resource "frida-core-devkit" do
    url "https://github.com/frida/frida/releases/download/16.1.2/frida-core-devkit-16.1.2-macos-arm64.tar.xz"
    sha256 "7811e516e6b7bbc0153d30095560e0b1133f154060c5542764100d3e0eb2ab2b"
  end

  resource "frida-gum-devkit" do
    url "https://github.com/frida/frida/releases/download/16.1.2/frida-gum-devkit-16.1.2-macos-arm64.tar.xz"
    sha256 "03f6085ae5330cf38e0a498784500675fc5bd7361bb551a9097ba5fe397aceda"
  end

  def install
    odie "bpftime v0.2.0's macOS Frida build is arm64-only" if OS.mac? && !Hardware::CPU.arm?

    mkdir_p "third_party/frida"
    resource("frida-core-devkit").fetch
    resource("frida-gum-devkit").fetch
    cp resource("frida-core-devkit").cached_download,
       "third_party/frida/frida-core-devkit-16.1.2-macos-arm64.tar.xz"
    cp resource("frida-gum-devkit").cached_download,
       "third_party/frida/frida-gum-devkit-16.1.2-macos-arm64.tar.xz"

    inreplace "CMakeLists.txt",
              'set(DEST_DIR "$ENV{HOME}/.bpftime")',
              "set(DEST_DIR \"#{libexec}\")"

    %w[
      CMakeLists.txt
      tools/cli/CMakeLists.txt
      tools/bpftimetool/CMakeLists.txt
      tools/aot/CMakeLists.txt
      vm/llvm-jit/cli/CMakeLists.txt
    ].each do |cmake_file|
      inreplace cmake_file, "DESTINATION ~/.bpftime", "DESTINATION #{libexec}" if File.exist?(cmake_file)
    end

    inreplace "runtime/agent/CMakeLists.txt",
              'target_link_options(bpftime-agent PUBLIC "-Wl,-export-dynamic")',
              <<~CMAKE.chomp
                if(NOT APPLE)
                  target_link_options(bpftime-agent PUBLIC "-Wl,-export-dynamic")
                endif()
              CMAKE

    inreplace "vm/compat/ubpf-vm/CMakeLists.txt",
              'target_link_options(bpftime_ubpf_vm PUBLIC "-Wl,--whole-archive" "$<TARGET_FILE:bpftime_ubpf_vm>" "-Wl,--no-whole-archive")',
              <<~CMAKE.chomp
                if(APPLE)
                  target_link_options(bpftime_ubpf_vm PUBLIC "-Wl,-force_load,$<TARGET_FILE:bpftime_ubpf_vm>")
                else()
                  target_link_options(bpftime_ubpf_vm PUBLIC "-Wl,--whole-archive" "$<TARGET_FILE:bpftime_ubpf_vm>" "-Wl,--no-whole-archive")
                endif()
              CMAKE

    ENV.append "LDFLAGS", "-L#{Formula["ncurses"].opt_lib}"
    ENV.append "CPPFLAGS", "-I#{Formula["boost"].opt_include}"

    args = %W[
      -DCMAKE_INSTALL_PREFIX=#{prefix}
      -DCMAKE_BUILD_TYPE=RelWithDebInfo
      -DBPFTIME_BUILD_WITH_LIBBPF=OFF
      -DBPFTIME_BUILD_KERNEL_BPF=OFF
      -DBUILD_BPFTIME_DAEMON=OFF
      -DBPFTIME_ENABLE_UNIT_TESTING=OFF
      -DBPFTIME_ENABLE_CUDA_ATTACH=OFF
      -DBPFTIME_ENABLE_IOURING_EXT=OFF
      -DBPFTIME_LLVM_JIT=OFF
      -DBPFTIME_UBPF_JIT=ON
      -DBoost_INCLUDE_DIR=#{Formula["boost"].opt_include}
      -DCMAKE_INSTALL_RPATH=#{libexec}
      -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON
    ]

    system "cmake", "-S", ".", "-B", "build", *args
    system "cmake", "--build", "build", "--parallel"
    system "cmake", "--install", "build"

    (bin/"bpftime").write <<~SH
      #!/bin/bash
      exec "#{libexec}/bpftime" --install-location "#{libexec}" "$@"
    SH
    chmod 0755, bin/"bpftime"

    bin.install_symlink libexec/"bpftimetool" if (libexec/"bpftimetool").exist?
  end

  def caveats
    <<~EOS
      This macOS formula installs bpftime runtime files in:
        #{opt_libexec}

      The bpftime wrapper automatically passes:
        --install-location #{opt_libexec}

      This build uses bpftime's non-Linux configuration:
        BPFTIME_BUILD_WITH_LIBBPF=OFF
        BPFTIME_BUILD_KERNEL_BPF=OFF
        BPFTIME_LLVM_JIT=OFF
        BPFTIME_UBPF_JIT=ON
    EOS
  end

  test do
    assert_match "Usage", shell_output("#{bin}/bpftime --help")
  end
end
