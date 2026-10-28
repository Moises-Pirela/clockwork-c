# Bundled X11 headers (Linux)

The Linux GUI build compiles `sokol_app.h` against these headers instead of
the system X11 development packages, so no `-dev` package is needed: not
`libx11-dev`, `x11proto-dev`, `libxext-dev`, `libxi-dev`, `libxfixes-dev` nor
`libxcursor-dev`. Only the runtime libraries `libX11.so.6`, `libXi.so.6` and
`libXcursor.so.1`, present on any desktop Linux, are required; the build
links them by file name when the development symlinks are absent.

The files are unmodified copies from the X.Org releases packaged by
Ubuntu 24.04 (libx11 1.8.7, xorgproto 2023.2, libxext 1.3.4, libxi 1.8.1,
libxfixes 6.0.0, libxcursor 1.2.1), laid out as `sokol_app.h` includes
them. Every file carries its own MIT/X11-style copyright and permission
notice (The Open Group, Digital Equipment Corporation, Silicon Graphics,
Keith Packard, Red Hat, Peter Hutterer); those notices are the license
terms and are preserved verbatim. Upstream: https://gitlab.freedesktop.org/xorg.

| File | Package |
| --- | --- |
| `X11/Xlib.h`, `Xutil.h`, `Xresource.h`, `XKBlib.h`, `cursorfont.h` | libx11 |
| `X11/X.h`, `Xatom.h`, `Xfuncproto.h`, `Xmd.h`, `Xosdefs.h`, `keysym.h`, `keysymdef.h`, `extensions/XI2.h`, `XKB.h`, `XKBstr.h`, `xfixeswire.h` | xorgproto |
| `X11/extensions/Xge.h` | libxext |
| `X11/extensions/XInput2.h` | libxi |
| `X11/extensions/Xfixes.h` | libxfixes |
| `X11/Xcursor/Xcursor.h` | libxcursor |

Windows (Direct3D 11 / Win32) and macOS (Metal / Cocoa) do not use these
files; their windowing dependencies come with the platform SDK.
