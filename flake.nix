{
  description = "A Nix-flake-based cmake & ninja development environment";

  inputs.nixpkgs.url = "github:nixos/nixpkgs/a45fa362d887f4d4a7157d95c28ca9ce2899b70e";

  outputs = {
    self,
    nixpkgs,
  }: let
    supportedSystems = ["x86_64-linux"];
    forEachSupportedSystem = f:
      nixpkgs.lib.genAttrs supportedSystems (system:
        f {
          pkgs = import nixpkgs {
            inherit system;
          };
        });
  in {
    devShells = forEachSupportedSystem ({pkgs}: {
      default = pkgs.mkShell {
        packages = with pkgs; [
          cmake
          ninja

          python3
        ];
      };
    });
  };
}
