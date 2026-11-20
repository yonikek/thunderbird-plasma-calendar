{ lib
, stdenv
, cmake
, ninja
, python3
, qt6
, kdePackages
}:

stdenv.mkDerivation {
  pname = "thunderbird-plasma-calendar";
  version = "0.2.0";
  src = lib.cleanSourceWith {
    # cleanSource drops .git, result symlinks and editor backups so that
    # unrelated changes do not alter the derivation.
    src = lib.cleanSource ../.;
    filter = path: _type:
      !(builtins.elem (baseNameOf (toString path)) [
        "build"
        "build-install"
        "_install"
        "result"
        "__pycache__"
      ]);
  };

  nativeBuildInputs = [
    cmake
    ninja
    kdePackages.extra-cmake-modules
    qt6.wrapQtAppsHook
    python3
  ];

  buildInputs = [
    qt6.qtbase
    qt6.qtdeclarative
    kdePackages.kcoreaddons
    kdePackages.kdeclarative
    kdePackages.kconfig
  ];

  cmakeGenerator = "Ninja";

  meta = {
    description = "Thunderbird calendar events for the KDE Plasma 6 Digital Clock";
    longDescription = ''
      A Thunderbird MailExtension and native-messaging host that exports calendar
      events to a KDE Plasma CalendarEvents plugin.
    '';
    license = lib.licenses.gpl2Plus;
    platforms = lib.platforms.linux;
    maintainers = [ ];
  };
}
