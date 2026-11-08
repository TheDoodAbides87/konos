#pragma once

// Single source of the app version; packaging/arch/PKGBUILD and the release
// script read it too.
#define VMP_VERSION "0.6.0"
#define VMP_CHANNEL "beta"
#define VMP_VERSION_LABEL VMP_VERSION " Beta"
#define VMP_APP_ID "vmp"

// The Omarchy build (qmake CONFIG+=omarchy, used for the Arch package) also
// follows the Omarchy desktop theme when it's there.
#define VMP_EDITION ""

// Where updates are published: a public repository that holds only releases
// (the source repository is private). The in-app update check reads the
// latest release from it.
#define VMP_RELEASES_REPO "TheDoodAbides87/vmp-releases"
