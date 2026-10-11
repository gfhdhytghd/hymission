{
  description = "Hymission compatibility with Hyprland development HEAD";
  inputs.hyprland.url = "github:hyprwm/Hyprland/main";
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
            version = "0.10.2";
            src = self;
            nativeBuildInputs = with pkgs; [ cmake pkg-config ];
            buildInputs = with pkgs; [ lua glib gtk4 libadwaita gtk4-layer-shell wayland ];
            cmakeFlags = [ "-DBUILD_TESTING=ON" ];
            meta = { description = "Mission Control overview for Hyprland development HEAD"; license = pkgs.lib.licenses.gpl3Only; };
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
