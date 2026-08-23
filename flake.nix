{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;

      # Parse deps.lock (KEY=VALUE lines, '#' comments) so the Nix build and
      # scripts/build-deps.sh share one source of truth for pinned revisions.
      lockEntries =
        map
          (line:
            let pair = nixpkgs.lib.splitString "=" line;
            in {
              name = builtins.head pair;
              value = nixpkgs.lib.concatStringsSep "=" (builtins.tail pair);
            })
          (builtins.filter
            (line: line != "" && !(nixpkgs.lib.hasPrefix "#" line))
            (nixpkgs.lib.splitString "\n" (builtins.readFile ./deps.lock)));
      lock = builtins.listToAttrs lockEntries;

      mkAasdkPackage = system:
        let pkgs = nixpkgs.legacyPackages.${system};
        in pkgs.stdenv.mkDerivation {
          pname = "aasdk";
          version = "2026-05-13";
          src = builtins.fetchGit {
            url = lock.AASDK_URL;
            rev = lock.AASDK_REV;
          };
          patches = [
            ./patches/aasdk/keep-config-descriptor-alive.patch
            ./patches/aasdk/relocatable-cert-install.patch
          ];
          nativeBuildInputs = with pkgs; [ cmake pkg-config protobuf ];
          buildInputs = with pkgs; [ boost183 libusb1 openssl protobuf ];
          # CMAKE_POLICY_VERSION_MINIMUM: aasdk bundles modules declaring
          # 'cmake_minimum_required(VERSION 3.0.0)', rejected by CMake >= 4.
          cmakeFlags = [
            "-DAASDK_TEST=OFF"
            "-DSKIP_BUILD_PROTOBUF=ON"
            "-DSKIP_BUILD_ABSL=ON"
            "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
          ];
        };

      # Shared dependency set for the package and dev shells. Pass
      # aasdk = null to omit it (see the no-aasdk shell).
      commonBuildInputs = { pkgs, aasdk ? null }:
        builtins.filter (v: v != null) (with pkgs; [
          bluez bluez.dev dbus systemd.dev glib openssl.dev boost183 libusb1
          ffmpeg-full ffmpeg-full.dev protobuf
          aasdk
        ]);

      mkPackage = system: checks:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in pkgs.stdenv.mkDerivation {
          pname = "aa2acp";
          version = "0.1.0";
          src = self;
          nativeBuildInputs = with pkgs; [ cmake pkg-config ];
          buildInputs = commonBuildInputs {
            inherit pkgs;
            aasdk = mkAasdkPackage system;
          };
          cmakeBuildType = "Debug";
          cmakeFlags = [ "-DBUILD_TESTING=${if checks then "ON" else "OFF"}" ];
          doCheck = checks;
          checkPhase = ''
            ctest --output-on-failure
          '';
          installPhase = ''
            install -Dm755 aa2acp $out/bin/aa2acp
          '';
          meta.mainProgram = "aa2acp";
        };
    in {
      packages = forAllSystems (system: {
        aasdk = mkAasdkPackage system;
        aa2acp = mkPackage system true;
        aa2acp-unchecked = mkPackage system false;
      });
      devShells = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in {
          default = pkgs.mkShell {
            buildInputs = commonBuildInputs {
              inherit pkgs;
              aasdk = mkAasdkPackage system;
            } ++ (with pkgs; [
              cmake clang-tools ninja gcc gnumake pkg-config git openssh rsync
            ]);
            shellHook = ''
              echo 'aa2acp development shell'
            '';
          };
          # Shell without the prebuilt aasdk package, used to test
          # scripts/build-deps.sh end to end (the non-Nix build path):
          #   nix develop .#no-aasdk --command ./scripts/build-deps.sh
          no-aasdk = pkgs.mkShell {
            buildInputs = commonBuildInputs {
              inherit pkgs;
              aasdk = null;
            } ++ (with pkgs; [
              cmake clang-tools ninja gcc gnumake pkg-config git
            ]);
            shellHook = ''
              echo 'aa2acp development shell (no prebuilt aasdk; run scripts/build-deps.sh)'
            '';
          };
        });
    };
}
