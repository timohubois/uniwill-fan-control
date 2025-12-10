# Maintainer: Timo Hubois <your-email@example.com>
pkgname=uniwill-fan-control-dkms
pkgver=0.1.0
pkgrel=1
pkgdesc="Silent fan control for TUXEDO/Uniwill laptops (DKMS)"
arch=('x86_64')
url="https://github.com/timohubois/uniwill-fan-control"
license=('GPL2')
depends=('dkms')
makedepends=('gcc')
source=("uniwill_fan.c"
        "dkms.conf"
        "Makefile"
        "uniwill-fanctl.c"
        "uniwill-fan.service")
sha256sums=('SKIP'
            'SKIP'
            'SKIP'
            'SKIP'
            'SKIP')

_dkms_name="uniwill-fan-control"

build() {
    gcc -Wall -Wextra -O2 -o uniwill-fanctl uniwill-fanctl.c
}

package() {
    # Install DKMS module source
    install -Dm644 uniwill_fan.c "$pkgdir/usr/src/$_dkms_name-$pkgver/uniwill_fan.c"
    install -Dm644 dkms.conf "$pkgdir/usr/src/$_dkms_name-$pkgver/dkms.conf"
    
    # Install Makefile for DKMS (simplified version for DKMS builds)
    install -Dm644 /dev/stdin "$pkgdir/usr/src/$_dkms_name-$pkgver/Makefile" << 'EOF'
obj-m += uniwill_fan.o
EOF
    
    # Install daemon
    install -Dm755 uniwill-fanctl "$pkgdir/usr/bin/uniwill-fanctl"
    
    # Install systemd service
    install -Dm644 uniwill-fan.service "$pkgdir/usr/lib/systemd/system/uniwill-fan.service"
    
    # Install module load config
    install -Dm644 /dev/stdin "$pkgdir/usr/lib/modules-load.d/uniwill-fan.conf" <<< "uniwill_fan"
}
