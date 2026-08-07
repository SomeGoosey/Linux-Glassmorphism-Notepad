THIS IS VIBE CODED WITH OPENCODE

# Linux-Glassmorphism-Notepad
A notepad in glassmorphism style for linux, requires kde plasma. 
Compiled with clang++ -O0 -Wno-deprecated-declarations notepad.cpp -o notepad $(pkg-config --cflags --libs gtk+-3.0) -lX11

Dependencies (runtime):

GTK+ 3 (libgtk-3) — all UI
X11 (libX11) — rounded-window shape and the KDE blur property (_KDE_NET_WM_BLUR_BEHIND_REGION) set via XChangeProperty
Build-time: pkg-config with the gtk+-3.0 module, plus a C++ stdlib.

