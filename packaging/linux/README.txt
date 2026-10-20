SST Rematch - Linux build

Needs: 64-bit Linux with glibc 2.38 or newer (Ubuntu 24.04+, Debian 13, Fedora 39+), a Vulkan 1.3 driver, and your own copy of the game files.

1. Copy the game's files into a folder named "assets" next to sst_rematch (the same assets folder the Windows build uses)
2. Start it with ./sst_rematch.sh (or ./sst_rematch from this folder)
3. Optional: ./install-desktop-entry.sh adds it to your application menu

Everything the game writes stays in this folder: settings (sst_settings.ini), saves, logs/ and cache/. The shaders come precompiled (shaders/); the few the game creates while running compile once on first use. If the game crashes, sst_crash.txt (and the newest file in logs/) says where: please send both.