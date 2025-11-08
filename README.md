<h2>ChucK-DMX: A DMX ChuGin for ChucK.</h2>

**ChucK-DMX (v0.1.0)** — [**ChucK-DMX API Reference**](https://ccrma.stanford.edu/~hoangben/ChucK-DMX/api/)
• [**Examples**](https://ccrma.stanford.edu/~hoangben/ChucK-DMX/examples/)
• [**Changelog**](https://ccrma.stanford.edu/~hoangben/ChucK-DMX/releases/VERSIONS)

## Installing ChucK-DMX

The easiest way to install ChucK-DMX is through ChuMP, ChucK's package manager. If you wish to build from source, see [Building ChucK-DMX from Source](#building-chuck-dmx-from-source).

1. Download ChucK version 1.5.5.0 or later. You can get the latest version [here](https://chuck.stanford.edu/release/).

2. To install ChucK-DMX, run the following command (which uses ChucK's new package manager, [ChuMP](https://chuck.stanford.edu/chump)):
```txt
chump install DMX
```

## Updating ChucK-DMX

To update to the latest version of ChucK-DMX, run the following command:

```txt
chump update DMX
```

## Running ChucK-DMX

### Minimal Example

If the ChucK-DMX ChuGin is properly loaded, the following example will run without errors:

```
// example.ck
@import "DMX"
```

Congrats, you now have ChucK-DMX properly installed!

## Learning ChucK-DMX
- Check out the [API Reference](https://ccrma.stanford.edu/~hoangben/ChucK-DMX/api/).
- Check out the [Examples](https://ccrma.stanford.edu/~hoangben/ChucK-DMX/examples/) to get started using ChucK-DMX.

## Building ChucK-DMX from Source

If you prefer to build ChucK-DMX manually instead of installing via ChuMP, follow these instructions:

### Prerequisites

- [CMake](https://cmake.org/download/) version 3.15 or higher
- A C++17 compatible compiler
- On **Windows**: Visual Studio 2022 (or newer) with MSVC toolchain
- On **Linux/macOS**: GCC or Clang toolchain

### Clone the repository

```
git clone https://github.com/Oran2009/ChucK-DMX.git
cd ChucK-DMX
```

### Create and enter the build directory

```
mkdir build
cd build
```

### Configure and build

- On **Windows** with Visual Studio:
    ```
    cmake .. -G "Visual Studio 17 2022" -A x64
    cmake --build . --config Release
    ```

- On **Linux/macOS**:
    ```
    cmake .. -DCMAKE_BUILD_TYPE=Release
    make
    ```

### Output

- The built ChuGin plugin (`DMX.chug`) will be located inside the build output directory, typically in:

  - Windows: `build/Release/DMX.chug`
  - Linux/macOS: `build/DMX.chug`

### Using the built ChuGin

Copy the generated `.chug` file to your ChucK `plugins` directory or specify the path when loading the plugin in your scripts.

## Authors

ChucK-DMX was created and is maintained by <a target="_blank" href="https://ccrma.stanford.edu/~hoangben/">Ben Hoang</a>.
