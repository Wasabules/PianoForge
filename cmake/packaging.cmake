# Linux packages of the application and its VST3 plugin, .deb and .rpm, made from the install rules (the components
# pianoforge and vst3; what JUCE installs for developers is left out):
#   cmake --build --preset linux-release
#   cd build/linux-gcc && cpack -G DEB        # or RPM
# A package runs where the C library is at least the build machine's (the application asks for the glibc it was built
# against): build it on the oldest distribution aimed at (Ubuntu 24.04: the CI's ubuntu-24.04 runner; Fedora: its own
# oldest release). Not yet for distribution: the licence (JUCE's AGPLv3, or a JUCE licence) is to be settled first,
# and the application plays nothing until a corpus or a profile is chosen.

set(PIANOFORGE_PACKAGE_CONTACT "PianoForge <pianoforge@localhost>" CACHE STRING "The packages' maintainer (Name <address>)")

set(CPACK_PACKAGE_NAME "pianoforge")
set(CPACK_PACKAGE_VENDOR "PianoForge")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "${PIANOFORGE_PACKAGE_CONTACT}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Concert grand piano simulator played from a MIDI keyboard")
set(CPACK_PACKAGE_DESCRIPTION "A calibrated hybrid modal engine played from a MIDI keyboard, with recording of every take, \
technician-level settings, a VST3 instrument, and a feed for OBS Studio. It plays a modal profile built from a sample \
bank with PianoForge's profile builder.")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_SET_DESTDIR ON) # (the install rules see a staging DESTDIR: no system cache made into the package)
set(CPACK_INSTALL_PREFIX "/usr") # (with DESTDIR, the prefix the files land under)
set(CPACK_COMPONENTS_ALL pianoforge vst3 lv2)
set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_RPM_COMPONENT_INSTALL ON)

# JUCE loads X11 at run time (dlopen): the package tools cannot see those libraries, they are named here. LAME (MP3),
# rtkit (a realtime audio thread) and PipeWire's JACK are used when present.
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION "sound")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libx11-6, libxext6, libxcursor1, libxinerama1, libxi6, libxrandr2, libxss1")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "libmp3lame0, rtkit")
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "pipewire-jack")

set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_GROUP "Applications/Multimedia")
set(CPACK_RPM_PACKAGE_AUTOREQ ON)
set(CPACK_RPM_PACKAGE_REQUIRES "libX11, libXext, libXcursor, libXinerama, libXi, libXrandr, libXScrnSaver")
set(CPACK_RPM_PACKAGE_SUGGESTS "lame-libs, rtkit, pipewire-jack-audio-connection-kit")
# The folders the system owns, not the package.
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    /usr/share/applications /usr/share/icons /usr/share/icons/hicolor /usr/share/icons/hicolor/32x32
    /usr/share/icons/hicolor/32x32/apps /usr/share/icons/hicolor/512x512 /usr/share/icons/hicolor/512x512/apps
    /usr/share/icons/hicolor/scalable /usr/share/icons/hicolor/scalable/apps /usr/lib/vst3 /usr/lib/lv2 /usr/share/metainfo)

include(CPack)
