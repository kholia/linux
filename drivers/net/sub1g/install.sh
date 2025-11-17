#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Quick installation script for CMT2300A driver on Raspberry Pi
#

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Print colored message
print_msg() {
    echo -e "${GREEN}==>${NC} $1"
}

print_error() {
    echo -e "${RED}Error:${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}Warning:${NC} $1"
}

# Check if running on Raspberry Pi
check_platform() {
    print_msg "Checking platform..."
    if [ ! -f /proc/device-tree/model ]; then
        print_error "Not running on Raspberry Pi?"
        exit 1
    fi

    MODEL=$(tr -d '\0' < /proc/device-tree/model)
    print_msg "Detected: $MODEL"

    if [[ ! "$MODEL" =~ "Raspberry Pi" ]]; then
        print_warning "This doesn't appear to be a Raspberry Pi"
        read -p "Continue anyway? (y/N) " -n 1 -r
        echo
        if [[ ! $REPLY =~ ^[Yy]$ ]]; then
            exit 1
        fi
    fi
}

# Check for required files
check_files() {
    print_msg "Checking required files..."

    if [ ! -f "cmt2300a.c" ]; then
        print_error "cmt2300a.c not found in current directory"
        exit 1
    fi

    if [ ! -f "cmt2300a.h" ]; then
        print_error "cmt2300a.h not found in current directory"
        exit 1
    fi

    print_msg "All required source files found"
}

# Install dependencies
install_deps() {
    print_msg "Installing build dependencies..."

    sudo apt update
    sudo apt install -y \
        build-essential \
        device-tree-compiler \
        bc

    print_msg "Dependencies installed"
}

# Create Makefile if needed
create_makefile() {
    if [ ! -f "Makefile" ]; then
        print_msg "Creating Makefile..."
        cat > Makefile << 'EOF'
# SPDX-License-Identifier: GPL-2.0
obj-m += cmt2300a.o
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

.PHONY: all clean install
EOF
    fi
}

# Build driver
build_driver() {
    print_msg "Building CMT2300A driver..."

    # make clean
    make

    if [ ! -f "cmt2300a.ko" ]; then
        print_error "Build failed - cmt2300a.ko not created"
        exit 1
    fi

    print_msg "Driver built successfully: cmt2300a.ko"
}

# Install driver
install_driver() {
    print_msg "Installing driver module..."

    sudo make install

    print_msg "Driver installed to /lib/modules/$(uname -r)/extra/"
}

# Compile device tree overlay
compile_overlay() {
    if [ -f "cmt2300a-overlay.dts" ]; then
        print_msg "Compiling device tree overlay..."

        dtc -@ -I dts -O dtb \
            -o cmt2300a.dtbo \
            cmt2300a-overlay.dts

        if [ -f "cmt2300a.dtbo" ]; then
            print_msg "Installing device tree overlay..."
            sudo cp cmt2300a.dtbo /boot/overlays/ 2>/dev/null || \
            sudo cp cmt2300a.dtbo /boot/firmware/overlays/
            print_msg "Device tree overlay installed"
        fi
    else
        print_warning "Device tree overlay source not found, skipping"
    fi
}

# Configure boot
configure_boot() {
    print_msg "Configuring boot settings..."

    # Try new location first, fall back to old
    CONFIG_FILE="/boot/firmware/config.txt"
    if [ ! -f "$CONFIG_FILE" ]; then
        CONFIG_FILE="/boot/config.txt"
    fi

    if [ ! -f "$CONFIG_FILE" ]; then
        print_error "Could not find config.txt"
        exit 1
    fi

    # Check if already configured
    if grep -q "dtoverlay=cmt2300a" "$CONFIG_FILE"; then
        print_msg "Device tree overlay already configured in $CONFIG_FILE"
    else
        print_msg "Adding device tree overlay to $CONFIG_FILE"
        echo "" | sudo tee -a "$CONFIG_FILE" > /dev/null
        echo "# CMT2300A Sub-GHz Radio" | sudo tee -a "$CONFIG_FILE" > /dev/null
        echo "dtoverlay=cmt2300a" | sudo tee -a "$CONFIG_FILE" > /dev/null
        print_msg "Boot configuration updated"
    fi
}

# Create udev rules
create_udev_rules() {
    print_msg "Creating udev rules for device permissions..."

    echo 'KERNEL=="sub1g_dev*", MODE="0666"' | sudo tee /etc/udev/rules.d/99-sub1g.rules > /dev/null

    sudo udevadm control --reload-rules

    print_msg "Udev rules created"
}

# Main installation
main() {
    echo ""
    echo "========================================"
    echo "  CMT2300A Driver Installation Script"
    echo "========================================"
    echo ""

    check_platform
    check_files

    # Ask for confirmation
    echo ""
    print_warning "This will install the CMT2300A driver on your system."
    read -p "Continue? (y/N) " -n 1 -r
    echo ""
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        print_msg "Installation cancelled"
        exit 0
    fi

    # install_deps
    create_makefile
    build_driver
    install_driver
    compile_overlay
    configure_boot
    create_udev_rules

    echo ""
    echo "========================================"
    print_msg "${GREEN}Installation Complete!${NC}"
    echo "========================================"
    echo ""
    print_msg "Next steps:"
    echo "  1. Connect your CMT2300A module (see wiring guide)"
    echo "  2. Reboot your Pi: sudo reboot"
    echo "  3. After reboot, check: ls -l /dev/sub1g_dev00"
    echo "  4. Test with: ./test_cmt2300a.py tx 'Hello World'"
    echo ""
    print_warning "A reboot is required to load the device tree overlay"
    echo ""
    read -p "Reboot now? (y/N) " -n 1 -r
    echo ""
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        print_msg "Rebooting..."
        sudo reboot
    fi
}

# Run main
main
