# Sourced by the build scripts. In: CC, AR, SDK, TAG (a name for the cache,
# per ABI and CPU), HERE (src/). TLS=mbedtls (default) or openssl.
# Out: TLS_SRC (sources to compile with the program), TLS_CFLAGS, TLS_LIBS.
# Mbed TLS is compiled once into build/<TAG>/libmbedtls.a and reused.
TLS=${TLS:-mbedtls}
case "$TLS" in
openssl)
    TLS_SRC="$HERE/tls/openssl.c"
    TLS_CFLAGS=""
    TLS_LIBS="-lssl -lcrypto"
    ;;
mbedtls)
    MB="$HERE/../third_party/mbedtls"
    CACHE="$HERE/../build/$TAG"
    TLS_CFLAGS="-DMBEDTLS_CONFIG_FILE=<mbedtls_config.h> -I$HERE/tls -I$MB/include"
    if [ ! -f "$CACHE/libmbedtls.a" ] || [ "$HERE/tls/mbedtls_config.h" -nt "$CACHE/libmbedtls.a" ]; then
        rm -rf "$CACHE/mbedtls"; mkdir -p "$CACHE/mbedtls"
        for c in "$MB"/library/*.c; do
            # -DMBEDTLS_TEST_SW_INET_PTON: the AROS C library has no inet_pton.
            $CC -O2 -std=gnu99 -w $TLS_CFLAGS -DMBEDTLS_TEST_SW_INET_PTON -I"$MB/library" -I"$SDK/include" \
                -c "$c" -o "$CACHE/mbedtls/$(basename "$c" .c).o" || exit 1
        done
        rm -f "$CACHE/libmbedtls.a"
        "$AR" rcs "$CACHE/libmbedtls.a" "$CACHE"/mbedtls/*.o
    fi
    # Randomness: ENTROPY=getentropy for a system that has a source
    # (mainline), jent (the default) for one that does not. Never both, and
    # never a fallback from one to the other.
    ENTROPY=${ENTROPY:-jent}
    TLS_SRC="$HERE/tls/mbedtls.c $HERE/tls/platform_aros.c"
    TLS_LIBS="$CACHE/libmbedtls.a"
    case "$ENTROPY" in
    getentropy)
        TLS_SRC="$TLS_SRC $HERE/tls/entropy_getentropy.c" ;;
    jent)
        JE="$HERE/../third_party/jitterentropy"
        TLS_CFLAGS="$TLS_CFLAGS -I$HERE/tls/jent-aros -I$JE"
        TLS_SRC="$TLS_SRC $HERE/tls/entropy_jent.c"
        if [ ! -f "$CACHE/libjent.a" ] || [ "$HERE/tls/jent-aros/jitterentropy-base-user.h" -nt "$CACHE/libjent.a" ]; then
            rm -rf "$CACHE/jent"; mkdir -p "$CACHE/jent"
            for c in "$JE"/src/*.c; do
                # -O0: upstream's requirement; jitterentropy-base.c refuses
                # to compile with optimisation. -std=gnu99 for the inline asm.
                $CC -O0 -std=gnu99 -fwrapv -w -I"$HERE/tls/jent-aros" -I"$JE" -I"$JE/src" -I"$SDK/include" \
                    -c "$c" -o "$CACHE/jent/$(basename "$c" .c).o" || exit 1
            done
            rm -f "$CACHE/libjent.a"
            "$AR" rcs "$CACHE/libjent.a" "$CACHE"/jent/*.o
        fi
        TLS_LIBS="$TLS_LIBS $CACHE/libjent.a" ;;
    *) echo "ENTROPY must be jent or getentropy"; exit 2 ;;
    esac
    ;;
*) echo "TLS must be mbedtls or openssl"; exit 2 ;;
esac
