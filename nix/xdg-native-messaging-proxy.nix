{ lib
, stdenv
, fetchurl
, meson
, ninja
, pkg-config
, glib
, json-glib
, libdex
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "xdg-native-messaging-proxy";
  version = "0.1.0";

  src = fetchurl {
    url = "https://github.com/flatpak/xdg-native-messaging-proxy/releases/download/${finalAttrs.version}/xdg-native-messaging-proxy-${finalAttrs.version}.tar.xz";
    hash = "sha256-QWfpQIsLB5ceT2lIYCz0ZOACsmlLskR+bAZyXCc9k8s=";
  };

  nativeBuildInputs = [
    meson
    ninja
    pkg-config
  ];

  buildInputs = [
    glib
    json-glib
    libdex
  ];

  meta = {
    description = "Expose host native messaging manifests to sandboxed applications over D-Bus";
    homepage = "https://github.com/flatpak/xdg-native-messaging-proxy";
    license = lib.licenses.lgpl2Plus;
    platforms = lib.platforms.linux;
    maintainers = [ ];
  };
})
