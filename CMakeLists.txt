cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Bluedroid sizes its security service table for one HFP role; we run both HF and AG,
# and an outgoing AG connection otherwise fails with "Out of Service Records"
idf_build_set_property(COMPILE_DEFINITIONS "BTM_SEC_MAX_SERVICE_RECORDS=8" APPEND)
# AG features: Bluedroid's default WBS set minus eSCO S4, i.e. ECNR | VREC | INBAND | REJECT |
# ECS | EXTERR | CODEC | UNAT. With S4 on both sides Bluedroid asks for a 2-EV3-only CVSD
# link, the Freecom 2x rejects it (Invalid LMP Parameters) and Bluedroid doesn't fall back.
# Numeric because PlatformIO doesn't escape parentheses in defines for src/ files.
idf_build_set_property(COMPILE_DEFINITIONS "BTC_HF_FEATURES=0x2036E" APPEND)
project(open-intercom)
