# Pyroveil

Pyroveil is a Vulkan layer that can automatically replace shaders or roundtrip them via SPIRV-Cross -> glslang.
This can create SPIR-V that is more compatible with NVIDIA drivers in particular.

## Checkout and build

Make sure to install `git`, `cmake`, `ninja` and gcc toolchains first (often called `base-devel` or similar).

```
git clone https://github.com/HansKristian-Work/pyroveil.git
cd pyroveil
git submodule update --init
cmake . -Bbuild -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$HOME/.local
ninja -C build install
```

Alternative path if using Steam Flatpak. (I assume this is correct).

```
cmake . -Bbuild -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$HOME/.var/app/com.valvesoftware.Steam/.local
```

## To use

The layer needs a config file to operate. See the folders under `hacks/` for some ready to go config files.
Use `PYROVEIL_CONFIG=/path/to/pyroveil.json` to pick the config you want.
In Steam, run the game with `PYROVEIL=1 %command%`.

To know that it's active, run `grep "pyroveil:" ~/steam-$appid.txt` on the `PROTON_LOG` output. You should see, e.g.:

```
pyroveil: Found config in /tmp/pyroveil.json!
pyroveil: Adding match for spirvExecutionModel: 5365 (MeshEXT).
pyroveil: Adding GLSL roundtrip via SPIRV-Cross for match.
...
pyroveil: Found match for execution model in 55736cd1c3064f67.
...
```

Some config files use a roundtripCache to provide patched SPIR-V.
In this case, `cache/` folder next to pyroveil.json must also be copied.
It is likely easier to simply use the path that you checked out for the `pyroveil.json`. E.g.

`PYROVEIL=1 PYROVEIL_CONFIG=/path/to/pyroveil/checkout/hacks/monster-hunter-wilds-nv/pyroveil.json`
