{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    nixpkgs-aasdk.url = "github:NixOS/nixpkgs/nixos-24.11";
    aasdk = {
      url = "github:opencardev/aasdk";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, nixpkgs-aasdk, aasdk }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
      mkAasdkPackage = system:
        let aasdkPkgs = nixpkgs-aasdk.legacyPackages.${system};
        in aasdkPkgs.stdenv.mkDerivation {
          pname = "aasdk";
          version = "2026-05-13";
          src = aasdk;
          nativeBuildInputs = with aasdkPkgs; [ cmake pkg-config protobuf ];
          buildInputs = with aasdkPkgs; [ boost183 libusb1 openssl protobuf ];
          postPatch = ''
            substituteInPlace CMakeLists.txt --replace-fail 'DESTINATION /etc/aasdk' 'DESTINATION etc/aasdk'
            substituteInPlace CMakeLists.txt \
              --replace-fail 'set(_cert_dir "$ENV{DESTDIR}/etc/aasdk")' \
              'set(_cert_dir "$ENV{DESTDIR}''${CMAKE_INSTALL_PREFIX}/etc/aasdk")'
            substituteInPlace include/aasdk/USB/AOAPDevice.hpp \
              --replace-fail 'DeviceHandle handle,' \
              'DeviceHandle handle, ConfigDescriptorHandle config_descriptor,'
            sed -i '/DeviceHandle handle_;/a\      ConfigDescriptorHandle config_descriptor_;' include/aasdk/USB/AOAPDevice.hpp
            substituteInPlace src/USB/AOAPDevice.cpp \
              --replace-fail 'DeviceHandle handle,' \
              'DeviceHandle handle, ConfigDescriptorHandle config_descriptor,' \
              --replace-fail 'handle_(std::move(handle)), interfaceDescriptor_(interfaceDescriptor)' \
              'handle_(std::move(handle)), config_descriptor_(std::move(config_descriptor)), interfaceDescriptor_(interfaceDescriptor)' \
              --replace-fail 'std::move(handle), interfaceDescriptor);' \
              'std::move(handle), std::move(configDescriptorHandle), interfaceDescriptor);'
          '';
          cmakeFlags = [ "-DAASDK_TEST=OFF" "-DSKIP_BUILD_PROTOBUF=ON" "-DSKIP_BUILD_ABSL=ON" ];
        };
      mkPackage = system: checks:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          aasdkPkgs = nixpkgs-aasdk.legacyPackages.${system};
        in pkgs.stdenv.mkDerivation {
          pname = "aa2acp";
          version = "0.1.1";
          src = self;
          nativeBuildInputs = with pkgs; [ cmake pkg-config ];
          buildInputs = with pkgs; [
            bluez bluez.dev dbus systemd.dev glib openssl.dev boost183 libusb1
            ffmpeg-full ffmpeg-full.dev aasdkPkgs.protobuf
            (mkAasdkPackage system)
          ];
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
          aasdkPkgs = nixpkgs-aasdk.legacyPackages.${system};
        in {
          default = pkgs.mkShell {
            buildInputs = with pkgs; [
              bluez bluez.dev dbus systemd.dev glib openssl.dev boost183 libusb1
              ffmpeg-full ffmpeg-full.dev aasdkPkgs.protobuf
              (mkAasdkPackage system)
              cmake clang-tools ninja gcc gnumake pkg-config git openssh rsync
            ];
            shellHook = ''
              echo 'aa2acp development shell'
            '';
          };
        });
    };
}
