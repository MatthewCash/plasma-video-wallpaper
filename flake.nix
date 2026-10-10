{
  description = "Minimal hw-accelerated video wallpaper for Plasma 6";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      forAll = nixpkgs.lib.genAttrs nixpkgs.lib.systems.flakeExposed;
    in {
      packages = forAll (system:
        let pkgs = nixpkgs.legacyPackages.${system}; in {
        default = pkgs.kdePackages.mkKdeDerivation {
          pname = "plasma-video-wallpaper";
          version = "1.0.0";
          src = ./.;

          extraNativeBuildInputs = with pkgs; [
            kdePackages.extra-cmake-modules
            pkg-config
          ];
          extraBuildInputs = with pkgs; [
            mpv-unwrapped
            libglvnd
            kdePackages.libplasma
            kdePackages.qtdeclarative
          ];
          cmakeFlags = [ "-DBUILD_TESTING=OFF" ];

          meta = with pkgs.lib; {
            description = "Minimal hw-accelerated video wallpaper";
            license = licenses.gpl2Plus;
            platforms = platforms.linux;
          };
        };
      });
      devShells = forAll (system:
        let pkgs = nixpkgs.legacyPackages.${system}; in {
          default = pkgs.mkShell {
            inputsFrom = [ self.packages.${system}.default ];
            packages = [ pkgs.python3 pkgs.ffmpeg ];
          };
        });
    };
}
