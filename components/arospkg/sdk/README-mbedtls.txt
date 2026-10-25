Mbed TLS 3.6.7 for AROS x86_64 (ABIv11), port revision aros2
============================================================

Static libraries of Mbed TLS 3.6.7 (the long-term support branch) for
programs built for AROS x86_64 with ABIv11: AROS One and the other current
distributions. Not for mainline AROS (ABIv1), i386 or aarch64: a library
built for one ABI does not work with another.

Upstream:  https://github.com/Mbed-TLS/mbedtls, release 3.6.7,
           mbedtls-3.6.7.tar.bz2, SHA-256
           a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
           The library sources are unchanged.
Port:      https://github.com/tomaszstaniak/arospkg (sdk/, src/tls/,
           third_party/mbedtls/); BUILD.txt names the commit.
Licences:  Mbed TLS: Apache-2.0 OR GPL-2.0-or-later (LICENSE), copyright The
           Mbed TLS Contributors.
           The AROS platform file, the configuration header, the example and
           the build script: MIT, copyright Tomasz Staniak (LICENSE.port).


Maintenance
-----------

These packages make the AROS ports developed for arospkg, a package manager
for AROS, available to other developers. Source changes, build scripts and
examples are included. No regular update schedule is promised;
contributions and upstream integration are welcome. Applications linking
these libraries remain responsible for tracking relevant upstream security
updates and rebuilding when necessary.


What is in it
-------------

  lib/libmbedcrypto.a   ciphers, hashes, public keys, random generators, PSA
  lib/libmbedx509.a     certificates
  lib/libmbedtls.a      the TLS protocol
  lib/libmbedtls_aros.a the AROS platform functions the configuration asks
                        for: mbedtls_ms_time() and mbedtls_platform_gmtime_r()
  include/              the upstream headers, and mbedtls_aros_config.h
  examples/https_get.c  a complete HTTPS client (see below)
  src/                  our platform file, the source lists and the build
                        script


The configuration is a TLS client profile
-----------------------------------------

The libraries were built with include/mbedtls_aros_config.h, and a program
MUST be compiled with the same file:

    -DMBEDTLS_CONFIG_FILE="<mbedtls_aros_config.h>"

It is the profile of a TLS client that verifies servers, nothing more:

  - TLS 1.2 and TLS 1.3, client side only; no server, no DTLS;
  - ECDHE key exchange (RSA and ECDSA certificates), curves P-256, P-384,
    P-521 and X25519;
  - AES-GCM and ChaCha20-Poly1305; SHA-1 (only for old certificates in a
    chain), SHA-224/256/384/512;
  - X.509 certificate parsing and verification, PEM and DER;
  - PSA Crypto (TLS 1.3 needs it);
  - mbedtls_strerror() for readable error messages;
  - no file system or socket layer of Mbed TLS's own (MBEDTLS_FS_IO and
    MBEDTLS_NET_C are off): read files with AmigaDOS and pass the data;
    connect with bsdsocket and pass send/recv (see the example).

If your program needs something else (a server, other ciphers, file
helpers), this profile is not for you; rebuild with your own configuration
using src/make-packages.sh as a starting point.


Entropy: you must connect a source
----------------------------------

Mbed TLS needs a source of unpredictable data to make keys. The C library's
rand() is NOT one. This configuration sets MBEDTLS_ENTROPY_HARDWARE_ALT and
MBEDTLS_NO_PLATFORM_ENTROPY, which means: your program must provide

    int mbedtls_hardware_poll(void *data, unsigned char *output,
                              size_t len, size_t *olen);

and the link fails without it. The companion package
JitterEntropy-3.7.0-aros2 provides this function from the jitterentropy
CPU-jitter source: link its libjitterentropy_mbedtls.a and
libjitterentropy.a.

A program may instead provide its own suitable entropy backend, such as a
system interface where the target provides one. Use one source, and do not
fall back from one to another. Source failures are reported to the caller.

Linking the libraries does not by itself make a program's TLS sound. Check
the return value of mbedtls_ctr_drbg_seed() and psa_crypto_init(): if the
source fails they fail, and the program must then not use TLS. The example
does exactly this.


Building a program
------------------

With MB and JE the directories of the two unpacked packages:

    x86_64-aros-gcc -O2 examples/https_get.c \
        -I$MB/include -I$JE/include \
        '-DMBEDTLS_CONFIG_FILE=<mbedtls_aros_config.h>' \
        -L$MB/lib -L$JE/lib \
        -lmbedtls -lmbedx509 -lmbedcrypto -lmbedtls_aros \
        -ljitterentropy_mbedtls -ljitterentropy \
        -o https_get

The order of the -l options matters: TLS, then X.509, then crypto, then the
AROS functions, then the entropy adapter and its library.


The example
-----------

    https_get <host> [path]

What it shows, in order:

  1. entropy: psa_crypto_init() and mbedtls_ctr_drbg_seed() are checked
     before anything else; if the source is unusable it stops with exit
     code 20 and makes no connection;
  2. trust: it loads the CA bundle from ENV:SYS/Certificates/ca-bundle.crt
     (present on AROS One 1.3; on another system install a PEM bundle
     there) and refuses to connect if no certificate in it parses;
  3. verification: MBEDTLS_SSL_VERIFY_REQUIRED, and
     mbedtls_ssl_set_hostname() so the certificate must be valid for <host>;
     a failed handshake prints mbedtls_ssl_get_verify_result() and the
     error text, and exits 10; so does a failure of any other TLS call
     (set-up, host name, write, read);
  4. it prints the negotiated protocol and the response's status line,
     sends close_notify, and frees every context, including the entropy
     collector (aros_jent_shutdown()).

It needs a running TCP/IP stack (bsdsocket.library).


Errors your program has to handle
---------------------------------

  - entropy: mbedtls_ctr_drbg_seed() or psa_crypto_init() failing;
  - trust: no CA bundle, or none of its certificates parsed;
  - the handshake: a certificate that does not verify, a wrong host name,
    an expired one; mbedtls_ssl_get_verify_result() says which;
  - the transport: your send/recv callbacks returning errors.

None of these should be retried with verification turned off.


Toolchain and rebuilding
------------------------

Built with GCC 10.5.0, the x86_64-aros cross toolchain made with
"make crosstools" from deadwood2/AROS (commit 5376f09bc1ed, 2026-08-17), and
the ABIv11 SDK's headers and link libraries. BUILD.txt has the details, the
source commit and the hash of each library file. The Mbed TLS sources used
are listed in src/mbedtls-sources.txt.

To rebuild both packages from source:

    git clone https://github.com/tomaszstaniak/arospkg
    cd arospkg
    git checkout <commit from BUILD.txt>
    TC=<directory of x86_64-aros-gcc> SDK=<ABIv11 SDK> sh sdk/make-packages.sh

The upstream sources are in third_party/mbedtls/ (library/, include/ and
LICENSE of the release named above). The ZIP files appear in dist/sdk/.


What was tested, and what is not claimed
----------------------------------------

Tested on AROS One 1.3 x86_64 under QEMU (TCG), with this revision's
example built only from the unpacked packages: a TLS 1.3 connection with a
verified certificate (exit 0), and refusal of an expired certificate, a
wrong host name, an untrusted root and a self-signed certificate (exit 10
each). The arospkg package manager, which uses the same libraries and
configuration, was also tested against plain HTTP, a redirect to HTTP and a
failed entropy source; each was refused. The reports are in docs/reports/
of the port's repository at the commit in BUILD.txt (2026-09-30-sdk-aros2,
2026-09-29-rc-tls-sdk and 2026-09-30-rc1-regression).

Not tested: other ABIv11 systems, real hardware, TLS servers other than
those in the reports. This is a build and a working example, not an audit
or a certification. For the limits of the entropy source, see the
JitterEntropy package's README.
