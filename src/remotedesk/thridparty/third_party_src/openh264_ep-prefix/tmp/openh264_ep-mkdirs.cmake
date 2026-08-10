# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "G:/remote/src/remotedesk/thridparty/third_party_src/openh264")
  file(MAKE_DIRECTORY "G:/remote/src/remotedesk/thridparty/third_party_src/openh264")
endif()
file(MAKE_DIRECTORY
  "G:/remote/src/remotedesk/thridparty/third_party_src/openh264_ep-prefix/src/openh264_ep-build"
  "G:/remote/src/remotedesk/thridparty/third_party_src/openh264_ep-prefix"
  "G:/remote/src/remotedesk/thridparty/third_party_src/openh264_ep-prefix/tmp"
  "G:/remote/src/remotedesk/thridparty/third_party_src/openh264_ep-prefix/src/openh264_ep-stamp"
  "G:/remote/src/remotedesk/thridparty/third_party_src/openh264_ep-prefix/src"
  "G:/remote/src/remotedesk/thridparty/third_party_src/openh264_ep-prefix/src/openh264_ep-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "G:/remote/src/remotedesk/thridparty/third_party_src/openh264_ep-prefix/src/openh264_ep-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "G:/remote/src/remotedesk/thridparty/third_party_src/openh264_ep-prefix/src/openh264_ep-stamp${cfgdir}") # cfgdir has leading slash
endif()
