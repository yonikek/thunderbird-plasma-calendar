{ config, lib, pkgs, ... }:
let
  cfg = config.programs.thunderbird-plasma-calendar;
  package = pkgs.callPackage ./package.nix { };
  nativeMessagingProxy = pkgs.callPackage ./xdg-native-messaging-proxy.nix { };
in
{
  options.programs.thunderbird-plasma-calendar = {
    enable = lib.mkEnableOption
      "Thunderbird calendar integration for the Plasma Digital Clock";

    flatpakProxy = lib.mkEnableOption ''
      the xdg-native-messaging-proxy host service for the Flatpak build of
      Thunderbird. Nixpkgs does not currently package this proxy.
    '';
  };

  config = lib.mkIf cfg.enable {
    environment.systemPackages = [ package ] ++ lib.optional cfg.flatpakProxy nativeMessagingProxy;

    systemd.user.services.xdg-native-messaging-proxy = lib.mkIf cfg.flatpakProxy {
      description = "Native messaging proxy for sandboxed applications";
      wantedBy = [ "graphical-session.target" ];
      partOf = [ "graphical-session.target" ];
      after = [ "graphical-session.target" ];
      serviceConfig = {
        Type = "dbus";
        BusName = "org.freedesktop.NativeMessagingProxy";
        ExecStart = "${nativeMessagingProxy}/libexec/xdg-native-messaging-proxy";
        Environment = [
          "XNMP_HOST_LOCATIONS=${package}/share/mozilla/native-messaging-hosts"
        ];
        Restart = "on-failure";
        RestartSec = 2;
      };
    };
  };
}
