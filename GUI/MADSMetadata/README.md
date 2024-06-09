# Metadata GUI

This sub-project allows to build a QT Gui for dealing with metadata. It allows to create and send custom metadata on the Miroscic network.

## Requirements

QT 6.x are required to build this project. The opensource version of QT is enough.

## Build

From the root of the subproject, run the following commands:

```bash
cmake -Bbuild -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake -Bbuild -DFETCHCONTENT_FULLY_DISCONNECTED=ON
```
As usual, after the first build you can disable the fetc content process to speed up next compilations.