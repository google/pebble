#!/bin/bash
# Copyright 2024 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.


# Configuration
#######################################

BUILD_TAG=$(git describe)

# This should be outside the build/ directory as we build multiple configurations
OUT_DIR=mfg_release

README_TEXT=\
"This zip archive contains the scripts and files needed for manufacturing testing and validation of
Robert. There are two directories within the archive: firmware and scripts.


The firmware directory contains three different images. The exact name of the files will be
prepended with a unique build id, but will otherwise be as follows:
+---------------------------+--------------------------------------------------------------------+
| File Name                 | Notes                                                              |
+---------------------------+--------------------------------------------------------------------+
| ${BUILD_TAG}_boot.hex  | The bootloader which should be loaded via the ST-Link               |
| ${BUILD_TAG}_fw.hex    | The main MFG firmware which should be loaded via the ST-Link        |
| ${BUILD_TAG}_fw.bin    | The main MFG firmware to be used to return boards to mfg after PRF  |
| ${BUILD_TAG}_prf.bin   | The PRF image to be loaded with pulse_flash_imaging.py              |
+---------------------------+--------------------------------------------------------------------+


The scripts directory contains many Python scripts which should be used during the manufacturing
process. Most of the Python scripts which are included should not be run directly, and are purely to
support the other scripts. Below is a list of the scripts which are intended to be run directly
during the manufacturing process.
+-------------------------+----------------------------------------------------------------------+
| File Name               | Notes                                                                |
+-------------------------+----------------------------------------------------------------------+
| pulse_flash_imaging.py  | Used to load the PRF image into the external flash on the board      |
| audio_recording.py      | Gets raw audio data from the watch for validation of the microphone  |
+-------------------------+----------------------------------------------------------------------+"

REQUIREMENTS_TEXT=\
"bitarray==0.8.1
pyserial==3.0.1"

# Exit this script if any line below fails
set -e

# Echo lines as we execute them
set -x


# Output directory / scripts
#######################################

# Clean out any existing release files
rm -rf ${OUT_DIR}

# Create the release directory
mkdir -p ${OUT_DIR}

# Create the README.txt file
echo "${README_TEXT}" > ${OUT_DIR}/README.txt

# Create the requirements.txt file
echo "${REQUIREMENTS_TEXT}" > ${OUT_DIR}/requirements.txt

# Copy the scripts we're interested into the output directory
mkdir -p ${OUT_DIR}/scripts
cp tools/hdlc.py                       ${OUT_DIR}/scripts/
cp tools/binutils.py                   ${OUT_DIR}/scripts/
cp tools/audio_recording.py            ${OUT_DIR}/scripts/
cp tools/prompt.py                     ${OUT_DIR}/scripts/
cp tools/serial_port_wrapper.py        ${OUT_DIR}/scripts/
cp tools/stm32_crc.py                  ${OUT_DIR}/scripts/
cp tools/pebble_tty.py                 ${OUT_DIR}/scripts/
cp tools/pebble_tty_pyftdi.py          ${OUT_DIR}/scripts/
cp tools/fw_binary_info.py             ${OUT_DIR}/scripts/
cp tools/insert_firmware_descr.py      ${OUT_DIR}/scripts/
cp tools/pulse_legacy_flash_imaging.py ${OUT_DIR}/scripts/pulse_flash_imaging.py
cp tools/accessory_flash_imaging.py    ${OUT_DIR}/scripts/
cp tools/accessory_imaging.py          ${OUT_DIR}/scripts/
mkdir ${OUT_DIR}/scripts/pulse
cp tools/pulse/*.py                    ${OUT_DIR}/scripts/pulse/
tar -xf tools/cobs.tar.gz -C           ${OUT_DIR}/scripts/


for BOARD in robert_es robert_evt; do
    # Build firmware
    #######################################

    # Build MFG FW (--nohash so we don't have to send our log hash tools)
    ./waf distclean configure --board=${BOARD} --nohash --mfg --internal_sdk_build \
        build_prf bundle_recovery

    FIRMWARE_OUT_DIR=${OUT_DIR}/firmware/${BOARD}
    mkdir -p ${FIRMWARE_OUT_DIR}

    BUILD_PREFIX=${BOARD}_${BUILD_TAG}

    cp build/prf/src/fw/tintin_fw.hex  ${FIRMWARE_OUT_DIR}/${BUILD_PREFIX}_fw.hex
    cp build/prf/src/fw/tintin_fw.bin  ${FIRMWARE_OUT_DIR}/${BUILD_PREFIX}_fw.bin

    # Build PRF
    ./waf distclean configure --board=${BOARD} --internal_sdk_build --release \
        build_prf bundle_recovery

    cp build/prf/src/fw/tintin_fw.bin  ${FIRMWARE_OUT_DIR}/${BUILD_PREFIX}_prf.bin

    # Copy this into the parent release directory so we don't accidentally throw it in the zip.
    # We'll want to save these as artifacts.
    cp build/prf/src/fw/tintin_fw.elf  ${OUT_DIR}/${BUILD_PREFIX}_fw.elf
    cp build/prf/recovery_*.pbz        ${OUT_DIR}/${BUILD_PREFIX}_prf.pbz


    # Bootloader images
    #######################################

    # Robert is still under development, so we have to use a wildcard. Add some code to make sure
    # we're not picking up multiple files.
    if [ "$BOARD" = "robert_evt" -o "$BOARD" = "robert_es" ]; then
      # Use the robert_evt bootloader for both robert_es and robert_evt
      bootloader=$(find bin/boot -name boot_robert_evt@*.hex)
      num_bootloaders=$(echo $bootloader | wc -w)
      if [ $num_bootloaders -gt 1 ]; then
        >&2 echo "More than 1 bootloader!"
        >&2 echo "$bootloader"
        exit 1
      fi
    else
      # We should handle this at the top, this is more to prevent us from forgetting to update this
      # set of if statements when adding a new board.
      echo "Unknown board" >&2
      exit 1
    fi

    cp ${bootloader} ${FIRMWARE_OUT_DIR}/${BUILD_PREFIX}_boot.hex
done

# Package it up
#######################################

cd $OUT_DIR && zip -r ${BUILD_TAG}.zip scripts/ firmware/ README.txt requirements.txt
