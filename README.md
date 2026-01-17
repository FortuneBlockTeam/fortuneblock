Fortuneblock
===========================

FortuneBlock is a community-driven project, and we welcome anyone to join us to make it stronger.


Compile
-------

Clone the code:
```bash
$ git clone https://github.com/FortuneBlockTeam/fortuneblock
```

Compile depends:


```bash
$ cd fortuneblock/depends
$ make -j4 # Choose a good -j value, depending on the number of CPU cores available
$ cd ..
```

Building Fortuneblock Core

```bash
$ ./autogen.sh
$ ./configure --prefix=`pwd`/depends/<host>
$ make
$ make install # optional
```

Please replace `<host>` with your local system's `host-platform-triplet`. The following triplets are usually valid:
- `i686-pc-linux-gnu` for Linux32
- `x86_64-pc-linux-gnu` for Linux64
- `i686-w64-mingw32` for Win32
- `x86_64-w64-mingw32` for Win64
- `x86_64-apple-darwin14` for MacOSX
- `arm-linux-gnueabihf` for Linux ARM 32 bit
- `aarch64-linux-gnu` for Linux ARM 64 bit

If you want to cross-compile for another platform, choose the appropriate `<host>` and make sure to build the
dependencies with the same host before.

License
-------

Fortuneblock is released under the terms of the MIT license. See [COPYING](COPYING) for more
information or see https://opensource.org/licenses/MIT.
