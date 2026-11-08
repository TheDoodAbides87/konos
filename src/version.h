#pragma once

// Single source of the app version; packaging/arch/PKGBUILD and the release
// script read it too.
#define KONOS_VERSION "0.7.7"
#define KONOS_CHANNEL "beta"
#define KONOS_VERSION_LABEL KONOS_VERSION " Beta"
#define KONOS_APP_ID "konos"

// The Omarchy build (qmake CONFIG+=omarchy, used for the Arch package) also
// follows the Omarchy desktop theme when it's there.
#define KONOS_EDITION ""

// Where updates are published: a public repository that holds only releases
// (the source repository is private). The in-app update check reads the
// latest release from it.
#define KONOS_RELEASES_REPO "TheDoodAbides87/konos-releases"
