# NixOS: an FHS shell with what Genode's build needs (see SETUP, step 2).
# Copy to the workspace directory (the parent of genode/), then 'nix-shell'.
# It mounts ./genode-tools as /usr/local/genode and ./goa as /usr/local/goa.
# (NIXPKGS_ALLOW_UNFREE=1 may be needed.)
# shell.nix                                   
{ pkgs ? import <nixpkgs> {} }:                                                                                            
(pkgs.buildFHSEnv {                                                                                                             
  name = "Genode-env";                                                                                              

  targetPkgs = pkgs: with pkgs; [
    git
    file
    elfutils
    elfutils.dev
    gnumake
    flex
    bison
    gnum4
    pkg-config
    zlib
    gperf
    e2tools
    gptfdisk
    ncurses5
    ncurses5.dev
    # Fix for GDB looking for ncursesw/ncurses.h
    (runCommand "ncursesw-compat-headers" { } ''
      mkdir -p $out/include/ncursesw
      # Symlink all headers from ncurses.dev into the ncursesw subdirectory
      for f in ${ncurses.dev}/include/*.h; do
        ln -s "$f" $out/include/ncursesw/
      done
      # Also symlink the ncurses subdirectory if it exists (for internal includes)
      if [ -d ${ncurses.dev}/include/ncurses ]; then
        ln -s ${ncurses.dev}/include/ncurses $out/include/ncursesw/
      fi
    '')
    expat
    expat.dev
    gnat
    gmp
    gmp.dev
    gcc13
    autoconf269
    autogen
    automake
    wget
    python3
    gnupg
    mtools
    e2fsprogs

    stdenv.cc.cc.lib
    glibc.dev
    linuxHeaders

    rpcsvc-proto
    expect
    SDL2.dev
    tcl
    bc
    qemu
    libisoburn
    libxml2
    mawk
    nawk
    bubblewrap
    bash-completion

    rustup
  ];                                                                                                              

  extraBuildCommands = ''
    mkdir -p $out/usr/local/genode
    mkdir -p $out/usr/local/goa
    
    mkdir -p $out/usr/share/bash-completion/completions
    ln -s /usr/local/goa/share/bash-completion/goa \
        $out/usr/share/bash-completion/completions/goa
    
    ln -s /usr/local/goa/bin/goa $out/usr/bin/goa
  '';

   runScript = "bash";

   extraBwrapArgs = [
        "--bind" (toString ./genode-tools) "/usr/local/genode"
        "--bind" (toString ./goa) "/usr/local/goa"
   ];

}).env
