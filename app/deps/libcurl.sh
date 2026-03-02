#!/usr/bin/env bash
set -ex
. $(dirname ${BASH_SOURCE[0]})/_init
process_args "$@"

VERSION=8.11.1
URL="https://curl.se/download/curl-$VERSION.tar.xz"
SHA256SUM=c7ca7db48b0909743eaef34250da02c19bc61d4f1dcedd6603f109409536ab56

PROJECT_DIR="curl-$VERSION"
FILENAME="$PROJECT_DIR.tar.xz"

cd "$SOURCES_DIR"

if [[ -d "$PROJECT_DIR" ]]
then
    echo "$PWD/$PROJECT_DIR" found
else
    get_file "$URL" "$FILENAME" "$SHA256SUM"
    tar xf "$FILENAME"  # First level directory is "$PROJECT_DIR"
fi

mkdir -p "$BUILD_DIR/$PROJECT_DIR"
cd "$BUILD_DIR/$PROJECT_DIR"

export CFLAGS='-O2'
export CXXFLAGS="$CFLAGS"

if [[ -d "$DIRNAME" ]]
then
    echo "'$PWD/$DIRNAME' already exists, not reconfigured"
    cd "$DIRNAME"
else
    mkdir "$DIRNAME"
    cd "$DIRNAME"

    conf=(
        --prefix="$INSTALL_DIR/$DIRNAME"
        --disable-ldap
        --disable-ldaps
        --disable-rtsp
        --disable-dict
        --disable-telnet
        --disable-tftp
        --disable-pop3
        --disable-imap
        --disable-smb
        --disable-smtp
        --disable-gopher
        --disable-mqtt
        --disable-manual
        --disable-docs
        --without-brotli
        --without-zstd
        --without-libidn2
        --without-librtmp
        --without-libpsl
        --without-nghttp2
    )

    if [[ "$LINK_TYPE" == static ]]
    then
        conf+=(
            --enable-static
            --disable-shared
        )
    else
        conf+=(
            --disable-static
            --enable-shared
        )
    fi

    if [[ "$BUILD_TYPE" == cross ]]
    then
        conf+=(
            --host="$HOST_TRIPLET"
        )
    fi

    if [[ "$HOST" == win* ]]
    then
        conf+=(
            --with-schannel
            --without-zlib
        )
    else
        conf+=(
            --with-openssl
        )
    fi

    "$SOURCES_DIR/$PROJECT_DIR"/configure "${conf[@]}"
fi

make -j
make install
