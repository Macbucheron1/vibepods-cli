{
  description = "VibePods CLI development shell (Makefile-based)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
      in {
        devShells.default = pkgs.mkShell {
          packages = with pkgs; [
            gnumake
            pkg-config
            qt6.qtbase
            qt6.qtconnectivity
          ];
        };

        packages.vibepods_cli = pkgs.stdenv.mkDerivation {
          pname = "vibepods-cli";
          version = "0.1.0";
          src = builtins.path {
            path = ./.;
            name = "vibepods-cli-src";
          };

          nativeBuildInputs = with pkgs; [
            gnumake
            pkg-config
            qt6.wrapQtAppsHook
          ];

          buildInputs = with pkgs; [
            qt6.qtbase
            qt6.qtconnectivity
          ];

          buildPhase = ''
            runHook preBuild
            make -C linux
            runHook postBuild
          '';

          installPhase = ''
            runHook preInstall
            make -C linux PREFIX=$out install
            runHook postInstall
          '';
        };

        packages.default = self.packages.${system}.vibepods_cli;
        apps.default = {
          type = "app";
          program = "${self.packages.${system}.vibepods_cli}/bin/vibepods-cli";
        };
        apps.daemon = {
          type = "app";
          program = "${self.packages.${system}.vibepods_cli}/bin/vibepods-daemon";
        };
      });
}
