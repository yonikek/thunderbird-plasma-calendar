{ config, lib, pkgs, ... }:
let
  cfg = config.programs.thunderbird-plasma-calendar;
  package = pkgs.callPackage ./package.nix { };
  nativeMessagingProxy = pkgs.callPackage ./xdg-native-messaging-proxy.nix { };

  # XNMP_HOST_LOCATIONS *replaces* the proxy's built-in search path, so list
  # the usual per-user and system-profile locations as well. Otherwise every
  # other native-messaging host (KeePassXC, Plasma Browser Integration, ...)
  # becomes invisible to sandboxed browsers. %h and %E are systemd user-unit
  # specifiers for $HOME and $XDG_CONFIG_HOME.
  hostLocations = lib.concatStringsSep ":" ([
    "%h/.mozilla/native-messaging-hosts"
    "%E/mozilla/native-messaging-hosts"
    "/run/current-system/sw/share/mozilla/native-messaging-hosts"
    "${package}/share/mozilla/native-messaging-hosts"
  ] ++ cfg.flatpakProxy.extraHostLocations);
in
{
  options.programs.thunderbird-plasma-calendar = {
    enable = lib.mkEnableOption
      "Thunderbird calendar integration for the Plasma Digital Clock";

    flatpakProxy = {
      enable = lib.mkEnableOption ''
        the xdg-native-messaging-proxy host service for the Flatpak build of
        Thunderbird. Nixpkgs does not currently package this proxy
      '';

      extraHostLocations = lib.mkOption {
        type = lib.types.listOf lib.types.str;
        default = [ ];
        example = [ "/etc/my-native-hosts" ];
        description = ''
          Additional directories containing native-messaging host manifests
          that the proxy should search, on top of the per-user directories,
          the system profile and this package's manifest directory.
        '';
      };
    };
  };

  config = lib.mkIf cfg.enable {
    environment.systemPackages = [ package ]
      ++ lib.optional cfg.flatpakProxy.enable nativeMessagingProxy;

    # Make the host manifest and the XPI available under
    # /run/current-system/sw/share/.
    environment.pathsToLink = [
      "/share/mozilla/native-messaging-hosts"
      "/share/thunderbird-plasma-calendar"
    ];

    systemd.user.services.xdg-native-messaging-proxy = lib.mkIf cfg.flatpakProxy.enable {
      description = "Native messaging proxy for sandboxed applications";
      wantedBy = [ "graphical-session.target" ];
      partOf = [ "graphical-session.target" ];
      after = [ "graphical-session.target" ];
      serviceConfig = {
        Type = "dbus";
        BusName = "org.freedesktop.NativeMessagingProxy";
        ExecStart = "${nativeMessagingProxy}/libexec/xdg-native-messaging-proxy";
        Environment = [ "XNMP_HOST_LOCATIONS=${hostLocations}" ];
        Restart = "on-failure";
        RestartSec = 2;
      };
    };
  };
}
