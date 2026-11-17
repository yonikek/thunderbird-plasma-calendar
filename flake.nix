{
  description = "Thunderbird calendar events in the KDE Plasma 6 Digital Clock";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in {
      nixosModules.default = import ./nix/nixos-module.nix;

      packages = forAllSystems (system:
        let pkgs = import nixpkgs { inherit system; }; in {
          default = pkgs.callPackage ./nix/package.nix { };
          thunderbird-plasma-calendar = pkgs.callPackage ./nix/package.nix { };
          xdg-native-messaging-proxy = pkgs.callPackage ./nix/xdg-native-messaging-proxy.nix { };
        });

      devShells = forAllSystems (system:
        let pkgs = import nixpkgs { inherit system; }; in {
          default = pkgs.mkShell {
            packages = with pkgs; [
              cmake
              ninja
              kdePackages.extra-cmake-modules
              python3
              qt6.qtbase
              qt6.qtdeclarative
              kdePackages.kcoreaddons
              kdePackages.kdeclarative
              kdePackages.kconfig
            ];
          };
        });
    };
}
