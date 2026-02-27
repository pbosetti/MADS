# How to compile MADS

> NOTE: It is rarely needed to compile MADS: start with the provided installers from <https://git.new/MADS>

## On Linux and macOS

### Requirements

You need the followings:

- clang
- git
- cmake
- Ninja
- Python3 and `pip install packaging`

On Linux, reference compiler is Clang. Be sure that after installing it, you also set it as default:

```bash
update-alternatives --set cc /usr/bin/clang
update-alternatives --set c++ /usr/bin/clang++
```

### Build

```bash
git clone https://github.com/pbosetti/MADS.git
cd MADS
# Optional:
# git switch <your desired branch>
cmake -Bbuild -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=products -DMADS_DIRECTOR=ON -GNinja -Wno-dev
cmake --build build -j6
```

On macOS, fat binaries can be compiled (for Intel and Silicon architectures) by doing:

```bash
cmake -Bbuild -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=products -DMADS_DIRECTOR=ON -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -GNinja -Wno-dev
```

> NOTE: the `mads-director` GUI won't compile on Raspbian. Be sure to set it `OFF` on that platform.

### Install

`cmake --install build` installs everything under `products`, then you can add `products/bin` to yur `PATH` and you're set. Set another `CMAKE_INSTALL_PREFIX` in the above config step if you want to install in another position. 

Alternatively, just run `cmake --build build -t package` to create an install package in `packages`.

## On Windows

### Requirements

Starting from MADS v2, build process on Windows is really similar to Unixes.

Prerequisites:

- Visual Studio 18
- cmake for Windows
- git for Windows
- Ninja (via [Chocolately](https://chocolatey.org/install): <https://github.com/ninja-build/ninja/wiki/Pre-built-Ninja-packages>)
- Python3 for Windows
- `pip install packaging` (needed by the build script for mongocxx)


### Build 

Proceed as follows:

```PowerShell
git clone https://github.com/pbosetti/MADS.git
cd MADS
# Optional:
# git switch <your desired branch>
cmake -Bbuild -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=products -DMADS_DIRECTOR=ON -GNinja -Wno-dev
cmake --build build -j6
```

> NOTE: It is strongly suggested to use Ninja as a generator rather than the default Visual Studio: the compile time can be greatly reduced.
> If you don't need the `mads-director` GUI, you can set `MADS_DIRECTOR=OFF`.

### Install

Run `cmake --build build -t package` to create an installer package in the `packages` directory.


### WSL2 networking and `mads-broker` command

WSL2 is fine for development and testing on a single machine both Windows and Linux agents. But if you want to connect from Windows to a broker running in WSL2, then you need to forward the ports used by the broker to the external network, for they normally are only exposed to the host Windows machine. To do that, you first have to note the local IP of the WSL2 virtual machine. Open a Powershell with administrator privileges and type the following commands:

```ps
netsh interface portproxy add v4tov4 listenport=9090 listenaddress=0.0.0.0 connectport=9090 connectaddress=$(wsl hostname -I)
netsh interface portproxy add v4tov4 listenport=9091 listenaddress=0.0.0.0 connectport=9091 connectaddress=$(wsl hostname -I)
netsh interface portproxy add v4tov4 listenport=9092 listenaddress=0.0.0.0 connectport=9092 connectaddress=$(wsl hostname -I)
```

Yes, three times, one per each port (9090, 9091, and 9092).

Then you have to enable the traffic on those ports through the firewall (if it is enabled). Type WIN-I to open the settings, then type firewall in the search field and select *Windows Defender Firewall* and click on *Advanced settings*. click on *Inbound rules* on the lef panel, then *New Rule*, select *Port*, click on *Next*, and in the *specific local ports* add `9090-9092`. Confirm and close. Now it should work.

**PLEASE NOTE**: the IP address of the WSL2 machine is not granted to remain the same upon reboots. So if suddenly the broker stops working, then check if the address has changed (`wsl hostname -I` from powershell) and execute again the above three commands.

**ALSO NOTE**: This is only needed when you have **external agents** that want to connect with a `mads-broker` running in a WSL2 environment. Conversely, if you want to connect from any agent running in a WSL2 instance to a broker running on an external Linux/Mac machine, there is no problem (provided that the other machine is reachable!)