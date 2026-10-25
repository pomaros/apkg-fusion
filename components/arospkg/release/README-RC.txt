arospkg 0.3.1-rc2 -- RELEASE CANDIDATE, for testing
===================================================

This is a test build, not a release. It is published so that it can be
tried on more systems than ours. Do not replace a working arospkg 0.3 with
it; test it on its own, as described below.

What changed from 0.3.1-rc1
---------------------------

A download whose connection breaks is now reported as such: "the
connection to <host> broke after N of M bytes: connection reset" (or the
TLS error), instead of "the download is the wrong size". The partial
download is still discarded and nothing reaches the cache; only the
message changed.

What changed from 0.3
---------------------

TLS moved from the SDK's OpenSSL 1.1.0h to Mbed TLS 3.6.7 (TLS 1.2 and 1.3),
compiled into the programs; they are about a quarter of the size. On
systems without a system entropy source (AROS One and the other ABIv11
systems, the i386 line, hosted aarch64) randomness comes from
jitterentropy-library 3.7.0; on mainline AROS from getentropy(). If the
entropy source is unusable, apkg refuses to connect; it never falls back to
anything else.

This candidate is NOT yet an approved security update: the release
regression and the analysis of the entropy source on our test configuration
are still in progress. See the release notes on GitHub for what was tested.

Targets
-------

Each archive is for one CPU and ABI; a binary for one does not run on
another. Which archive was run on which system, and what was checked, is
in the release notes on GitHub: this file is written before the tests and
does not claim any. An archive the notes do not list as run is
experimental and not runtime-tested.

  x86_64-aros-v11   apkg and PkgManager   AROS One and other ABIv11 systems
  x86_64-aros-v1    apkg                  mainline AROS
  i386-aros-v0      apkg                  the i386 ABIv0 line
  aarch64-aros      apkg                  hosted aarch64 (Macaros); needs a
                                          posixc.library built for it

Testing it without touching your installation
---------------------------------------------

Unpack the archive anywhere (not over C: or SYS:Packages), then from a
Shell in that drawer:

    apkg --version
    apkg --root RAM:RcTest update
    apkg --root RAM:RcTest search
    apkg --root RAM:RcTest show zaphod
    apkg --root RAM:RcTest install zaphod
    apkg --root RAM:RcTest list
    apkg --root RAM:RcTest remove zaphod

--root keeps everything (index, cache, installed programs) in RAM:RcTest,
away from SYS:Packages and from an installed arospkg 0.3. Nothing is
written to C:, LIBS: or SYS:Packages. Delete RAM:RcTest when done. RAM:
is empty after a reboot; to test anything across a reboot, use a drawer
on a disk instead (for example --root SYS:RcTest).

It needs a running TCP/IP stack and the certificate bundle
ENV:SYS/Certificates/ca-bundle.crt. The catalogue offers x86_64 ABIv11
packages only; on other targets search hides them and install refuses them,
which is itself worth reporting.

Please report: the system and its version, the archive's name and the
output of apkg --version, what you ran and what it printed. Issues:
https://github.com/tomaszstaniak/arospkg/issues

Licences
--------

arospkg: MIT (LICENSE). Included: Mbed TLS 3.6.7 (Apache-2.0),
jitterentropy-library 3.7.0 (BSD-3-Clause; not in the mainline archive),
zlib; see licenses/.
