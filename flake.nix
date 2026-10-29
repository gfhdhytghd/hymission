{
  description = "Hymission for Hyprland v0.57.0";
  inputs.hyprland.url = "github:hyprwm/Hyprland/v0.57.0";
  inputs.nixpkgs.follows = "hyprland/nixpkgs";
  outputs = { self, hyprland, nixpkgs, ... }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      eachSystem = nixpkgs.lib.genAttrs systems;
    in {
      packages = eachSystem (system:
        let
          pkgs = import nixpkgs {
            inherit system;
            overlays = [ hyprland.overlays.hyprland-packages ];
          };
          hymission = pkgs.hyprlandPlugins.mkHyprlandPlugin {
            pluginName = "hymission";
            version = "0.12.0";
            src = self;
            # python3 registers the hook ABI type check (tools/verify_dev_hooks.py --types-only).
            nativeBuildInputs = with pkgs; [ cmake pkg-config python3 ];
            buildInputs = with pkgs; [ lua glib gtk4 libadwaita gtk4-layer-shell wayland ];
            # LD_PRELOAD probes need compositor symbols, absent in Nix's ELF wrapper.
            cmakeFlags = [
              "-DBUILD_TESTING=ON"
              "-DHYPRLAND_TEST_EXECUTABLE=${pkgs.hyprland}/bin/.Hyprland-wrapped"
            ];
            meta = { description = "Mission Control overview for Hyprland v0.57.0"; license = pkgs.lib.licenses.gpl3Only; };
            doCheck = true;
            checkPhase = ''
              runHook preCheck
              ctest --output-on-failure
              runHook postCheck
            '';
          };
        in { inherit hymission; default = hymission; });
      checks = eachSystem (system: { inherit (self.packages.${system}) hymission; });
    };
}
