# Build on Windows

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

