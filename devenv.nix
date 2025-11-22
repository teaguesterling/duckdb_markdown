{ pkgs, lib, config, inputs, ... }:

{
  # https://devenv.sh/packages/
  packages = [ pkgs.git pkgs.gnumake pkgs.ninja pkgs.pkgconf pkgs.autoconf pkgs.automake ];

  # https://devenv.sh/languages/
  languages.rust.enable = true;
  languages.python.enable = true;
}
