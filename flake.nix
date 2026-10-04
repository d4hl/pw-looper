{
  description = "pw-looper — Qt6 PipeWire loopback utility";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in
    {
      devShells = forAllSystems (system:
        let pkgs = nixpkgs.legacyPackages.${system}; in
        {
          default = pkgs.mkShell {
            packages = with pkgs; [
              cmake
              ninja
              pkg-config
              qt6.qtbase # dev output provides Qt6 CMake configs
              pipewire
              clang-tools # clang-format / clang-tidy / clangd
              dpkg # cpack DEB generator
              rpm  # cpack RPM generator
            ];
          };
        });

      packages = forAllSystems (system:
        let pkgs = nixpkgs.legacyPackages.${system}; in
        {
          default = pkgs.stdenv.mkDerivation {
            pname = "pw-looper";
            version = self.shortRev or "dirty"; # no .git in flake source, tag is unreachable
            src = nixpkgs.lib.cleanSourceWith {
              src = self;
              filter = path: type:
                !(type == "directory" && baseNameOf path == "build");
            };
            nativeBuildInputs = with pkgs; [
              cmake
              ninja
              pkg-config
              qt6.wrapQtAppsHook
            ];
            buildInputs = with pkgs; [
              qt6.qtbase
              pipewire
            ];
            postInstall = ''
              substituteInPlace $out/share/applications/pw-looper.desktop \
                --replace-fail 'Exec=pw-looper' "Exec=$out/bin/pw-looper"
            '';
          };
        });

      apps = forAllSystems (system: {
        default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/pw-looper";
        };
      });
    };
}