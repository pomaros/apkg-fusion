jitterentropy-library 3.7.0 for AROS x86_64 (ABIv11), port revision aros2
=========================================================================

Static libraries of jitterentropy-library, a CPU-jitter entropy source, for
programs built for AROS x86_64 with ABIv11, such as AROS One. With the
optional adapter it supplies the entropy that the companion Mbed TLS package
needs. These binaries are not for mainline AROS (ABIv1), i386 or aarch64: a
library built for one ABI does not work with another.

Upstream:  https://github.com/smuellerDD/jitterentropy-library, release
           3.7.0, v3.7.0.tar.gz, SHA-256
           f5eaccc9d2977c83308651be9379f09f34348398f419e8f8b5bbd95928c777ed
           The library sources are unchanged.
Port:      https://github.com/tomaszstaniak/arospkg (sdk/, src/tls/,
           third_party/jitterentropy/); BUILD.txt names the commit.
Licences:  jitterentropy-library: BSD-3-Clause or GPL-2.0, copyright
           Stephan Mueller (LICENSE, LICENSE.bsd, LICENSE.gplv2); this port
           uses it under the BSD licence.
           The AROS platform header, the Mbed TLS adapter, the example and
           the build script: MIT, copyright Tomasz Staniak (LICENSE.port).


Maintenance
-----------

These packages make the AROS ports developed for arospkg, a package manager
for AROS, available to other developers. Source changes, build scripts and
examples are included. No regular update schedule is promised;
contributions and upstream integration are welcome. Applications linking
these libraries remain responsible for tracking relevant upstream security
updates and rebuilding when necessary.


What is in it: three layers
---------------------------

1. The upstream library, unchanged:

     lib/libjitterentropy.a      compiled with -O0, as upstream requires
                                 (jitterentropy-base.c refuses to build
                                 otherwise), without the internal timer
                                 thread (JENT_CONF_ENABLE_INTERNAL_TIMER off)
     include/jitterentropy.h     upstream's interface

2. The AROS platform layer, ours:

     include/jitterentropy-base-user.h
         replaces upstream's file of that name, which uses POSIX and Linux
         facilities AROS does not have. It gives the library a time stamp
         (the CPU's time stamp counter, RDTSC), memory that is cleared on
         release, one CPU and an unknown cache size (so the library uses its
         default memory size). It must be the only file of that name the
         compiler finds.

3. The Mbed TLS adapter, ours, optional:

     lib/libjitterentropy_mbedtls.a
         provides mbedtls_hardware_poll(), aros_jent_status() and
         aros_jent_shutdown(). It uses no Mbed TLS header, so it does not
         depend on a particular Mbed TLS configuration.

   Also included: examples/jent_read.c (the library used directly) and
   src/ (the adapter's source, the platform header, the build script).


The clock, and what happens when it is not good enough
------------------------------------------------------

At jent_entropy_init() the library measures whether the time stamp counter
varies enough between repeated operations. If it does not, it returns an
error, and the library must not be used on that machine. The platform layer
and the adapter never override that answer and tune nothing.

Under QEMU without KVM the time stamp counter moves in coarse steps (in the
measurement below, every delta was a multiple of 1000). The library accepted
it on AROS One 1.3 under QEMU. Whether it does on another machine is decided
on that machine, at run time.

There is no fallback. If the start-up test, an allocation or a health test
fails, the adapter reports failure and returns no data; Mbed TLS then fails
to seed its generator, and a program that checks the return values makes no
connection. Nothing falls back to rand(), the time or a counter.


Using it with Mbed TLS
----------------------

The companion package MbedTLS-3.6.7-aros2 is configured with
MBEDTLS_ENTROPY_HARDWARE_ALT, so it takes its entropy from
mbedtls_hardware_poll(). Link, after the Mbed TLS libraries:

    -ljitterentropy_mbedtls -ljitterentropy

The adapter creates one collector per program on first use and serialises
access to it with an exec semaphore, so several tasks of one program may use
TLS. The start-up test runs at the first mbedtls_hardware_poll(), which Mbed
TLS makes while seeding (mbedtls_ctr_drbg_seed(), psa_crypto_init()).
aros_jent_status() does not run it; it reports its result: 1 before the
first use, 0 when the source is usable, jitterentropy's error code or -1
(collector not allocated) when it is not. Use it after a failed seed to say
why. A failed read (health test) wipes the output and fails that call; the
next call reads again. Call aros_jent_shutdown() once at the end of the
program to free the collector. The declarations are:

    int  aros_jent_status(void);
    void aros_jent_shutdown(void);

A program may instead connect another suitable entropy source, such as a
system interface where the target provides one. Use one source, and do not
fall back from one to another.


Using the library directly
--------------------------

    x86_64-aros-gcc -O2 examples/jent_read.c -I$JE/include -L$JE/lib \
        -ljitterentropy -o jent_read

jent_entropy_init(), jent_entropy_collector_alloc(), jent_read_entropy(),
jent_entropy_collector_free(): see include/jitterentropy.h. Check every
return value. A failed health test is reported as a negative return from
jent_read_entropy(), and the data must then not be used.


Toolchain and rebuilding
------------------------

Built with GCC 10.5.0, the x86_64-aros cross toolchain made with
"make crosstools" from deadwood2/AROS (commit 5376f09bc1ed, 2026-08-17), and
the ABIv11 SDK's headers and link libraries. BUILD.txt has the details, the
source commit and the hash of each library file.

To rebuild both packages from source:

    git clone https://github.com/tomaszstaniak/arospkg
    cd arospkg
    git checkout <commit from BUILD.txt>
    TC=<directory of x86_64-aros-gcc> SDK=<ABIv11 SDK> sh sdk/make-packages.sh

The upstream sources are in third_party/jitterentropy/ (README.arospkg there
names the tarball and its checksum). The ZIP files appear in dist/sdk/.


What was tested, and what is not claimed
----------------------------------------

Tested on AROS One 1.3 x86_64 under QEMU (TCG):

  - the examples of both packages, built only from the unpacked packages:
    jent_read returned data; the Mbed TLS example made a TLS 1.3 connection
    with a verified certificate and refused expired, wrong-host, untrusted
    and self-signed ones;
  - the arospkg package manager, which uses the same libraries and adapter;
  - a raw-noise measurement with upstream's own procedure
    (tests/raw-entropy in the release; 1,000,000 samples, NIST SP 800-90B
    non-IID estimator, 8-bit symbols): 2.21 bits per sample, against the
    0.333 the library's default oversampling needs. The runtime criterion is
    met in that configuration.

Not established:

  - the restart part of that procedure: the series stopped at 470 of 1000
    restarts and was not evaluated, so the SP 800-90B style assessment is
    not complete;
  - any other configuration: one machine, one boot, an idle guest; not
    measured under KVM, on real hardware, under load or after restoring a
    snapshot.

This is a port and a build, not a certification. The reports are in
docs/reports/ of the port's repository at the commit in BUILD.txt
(2026-09-29-jitterentropy, 2026-09-30-sdk-aros2 and 2026-09-29-rc-tls-sdk).
