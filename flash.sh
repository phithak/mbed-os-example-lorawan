#!/usr/bin/bash

DUT_LABEL="DIS_L072Z"
DUT_BOARD_NAME="DISCO-L072CZ-LRWAN1"
DUT_GREP=$(mbed-tools detect|grep "$DUT_BOARD_NAME")
DUT_SERIAL_PORT=$(echo $DUT_GREP| awk -F ' ' '{print $3}')
DUT_MOUNT_POINT=$(echo $DUT_GREP| awk -F ' ' '{print $4}')
DUT_BUILD_TARGET=$(echo $DUT_GREP| awk -F ' ' '{print $5}')
DUT_UUID=$(sudo blkid | grep "$DUT_LABEL" | awk -F '"' '{print $8}')

echo "BOARD INFO"
echo "=========="
echo "NAME: $DUT_BOARD_NAME"
echo "LABEL: $DUT_LABEL"
echo "UUID: $DUT_UUID"
echo "SERIAL_PORT: $DUT_SERIAL_PORT"
echo "MOUNT_POINT: $DUT_MOUNT_POINT"
echo "BUILD_TARGET: $DUT_BUILD_TARGET"

TOOLCHAIN="GCC_ARM"
MANUAL_MOUNT_POINT="/mnt/stm32"

echo
echo "FLASH_CMD:"
MKDIR_CMD="sudo mkdir -p $MANUAL_MOUNT_POINT"
echo $MKDIR_CMD
$MKDIR_CMD
UMOUNT_CMD="sudo umount /dev/disk/by-uuid/$DUT_UUID"
echo $UMOUNT_CMD
$UMOUNT_CMD
MOUNT_CMD="sudo mount -U $DUT_UUID $MANUAL_MOUNT_POINT"
echo $MOUNT_CMD
$MOUNT_CMD
FLASH_CMD="sudo cp -vf cmake_build/$DUT_BUILD_TARGET/develop/$TOOLCHAIN/*.bin $MANUAL_MOUNT_POINT"
echo $FLASH_CMD
$FLASH_CMD
echo
echo "Please wait until the firmware flashing process is complete."
