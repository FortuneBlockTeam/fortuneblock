Fortuneblock
===========================

FortuneBlock is a community-driven project, and we welcome anyone to join us to make it stronger.


Core Features
-------------

No Pre-Mining or Instant Mining: 

We adhere to the principle of fairness, with all tokens generated through mining after the blockchain network launches. By eliminating pre-mining and instant mining mechanisms, we ensure that all participants have equal opportunities to acquire tokens, preventing early investors from gaining an unfair advantage.

No Developer Fees: 

We commit to not charging any developer fees throughout the project. All profits will be directly distributed to miners and token holders, ensuring that every participant benefits from the network's success.

No ICO: 

To avoid speculation and price manipulation, we have chosen not to conduct an Initial Coin Offering (ICO). This decision emphasizes our transparency and commitment to the community, allowing everyone an equal opportunity to participate.

Unique Fortuneblock Algorithm: 

Our network employs a unique Fortuneblock algorithm aimed at enhancing transaction processing speed and security. This algorithm optimizes block generation times while introducing innovative mechanisms to bolster the network's resistance to attacks, ensuring that users' transactions are always secure and fast.

Community-Driven: Our development roadmap will be shaped collectively by the community. User feedback and needs will directly influence future features and improvements, ensuring the currency meets real-world usage requirements.

Vision
------
Our vision is to develop this new cryptocurrency into a mainstream payment solution, widely used in everyday transactions. We believe that with its transparent, fair, and secure characteristics, this currency will earn users' trust and support worldwide.

Join Us
-------

We invite everyone interested in cryptocurrency to join our community and witness the development and growth of this new payment method. Whether you are a miner, developer, or a regular user, your participation will be key to our success.

Let's work together to drive the future of finance and create a payment currency that everyone can use!

Compile
-------

Clone the code:
```bash
$ git clone https://github.com/FortuneBlockTeam/fortuneblock
```

Compile depends:


```bash
$ cd depends
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
