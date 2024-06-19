# Windows Notes

# WSL2 networking

WSL2 is fine for development and testing on a single machine. But if you want to connect to a broker running in WSL2, then you need to forward the ports used by the broker to the external network, for they normally are only exposed to the host Windows machine. To do that, you first have to note the local IP of the WSL2 virtual machine. Open a Powershell with administrator privileges and type the following commands:

```ps
netsh interface portproxy add v4tov4 listenport=9090 listenaddress=0.0.0.0 connectport=9090 connectaddress=$(wsl hostname -I)
netsh interface portproxy add v4tov4 listenport=9091 listenaddress=0.0.0.0 connectport=9091 connectaddress=$(wsl hostname -I)
netsh interface portproxy add v4tov4 listenport=9092 listenaddress=0.0.0.0 connectport=9092 connectaddress=$(wsl hostname -I)
```

Yes, three times, one per each port (9090, 9091, and 9092).

Then you have to enable the traffic on those ports through the firewall (if it is enabled). Type WIN-I to open the settings, then type firewall in the search field and select *Windows Defender Firewall* and click on *Advanced settings*. click on *Inbound rules* on the lef panel, then *New Rule*, select *Port*, click on *Next*, and in the *specific local ports* add `9090-9092`. Confirm and close. Now it should work.


## Build how-to

Prerequisites:

- Visual Studio Professional 2022
- cmake for Windows
- git for Windows

Proceed as follows:

- open cmake-gui and select `build` as build folder
- click on configure
- select Visual Studio 2022 as generator
- generate the project
- select the `FETCHCONTENT` category end tick `FETCHCONTENT_FULLY_DISCONNECTED`
- click on generate again
- open the Developer PowerShell and move into project directory

Now issue the following command:

```PowerShell
cmake --build build --config Release
```

The first build also downloads and compiles dependencies. From the second time, you can select `MADS_SKIP_EXTERNALS` to speed up things a lot.

Sometimes, you will get some linking errors for the project binaries: run again the compile command `cmake --build build --config Release` and they will vanish.

