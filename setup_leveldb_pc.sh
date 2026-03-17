#!/usr/bin/env bash
sudo bash -c 'cat > /usr/lib/x86_64-linux-gnu/pkgconfig/libleveldb.pc << EOF
prefix=/usr
exec_prefix=\${prefix}
libdir=\${prefix}/lib/x86_64-linux-gnu
includedir=\${prefix}/include

Name: libleveldb
Description: LevelDB library
Version: 1.23
Libs: -L\${libdir} -lleveldb
Cflags: -I\${includedir}
EOF'
echo "LevelDB pkg-config file created successfully."
