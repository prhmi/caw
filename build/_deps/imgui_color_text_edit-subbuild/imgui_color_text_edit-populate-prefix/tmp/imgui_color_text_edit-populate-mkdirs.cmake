# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "D:/myWork/codes/cpp/caw/17_score_4/build/_deps/imgui_color_text_edit-src")
  file(MAKE_DIRECTORY "D:/myWork/codes/cpp/caw/17_score_4/build/_deps/imgui_color_text_edit-src")
endif()
file(MAKE_DIRECTORY
  "D:/myWork/codes/cpp/caw/17_score_4/build/_deps/imgui_color_text_edit-build"
  "D:/myWork/codes/cpp/caw/17_score_4/build/_deps/imgui_color_text_edit-subbuild/imgui_color_text_edit-populate-prefix"
  "D:/myWork/codes/cpp/caw/17_score_4/build/_deps/imgui_color_text_edit-subbuild/imgui_color_text_edit-populate-prefix/tmp"
  "D:/myWork/codes/cpp/caw/17_score_4/build/_deps/imgui_color_text_edit-subbuild/imgui_color_text_edit-populate-prefix/src/imgui_color_text_edit-populate-stamp"
  "D:/myWork/codes/cpp/caw/17_score_4/build/_deps/imgui_color_text_edit-subbuild/imgui_color_text_edit-populate-prefix/src"
  "D:/myWork/codes/cpp/caw/17_score_4/build/_deps/imgui_color_text_edit-subbuild/imgui_color_text_edit-populate-prefix/src/imgui_color_text_edit-populate-stamp"
)

set(configSubDirs Debug)
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "D:/myWork/codes/cpp/caw/17_score_4/build/_deps/imgui_color_text_edit-subbuild/imgui_color_text_edit-populate-prefix/src/imgui_color_text_edit-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "D:/myWork/codes/cpp/caw/17_score_4/build/_deps/imgui_color_text_edit-subbuild/imgui_color_text_edit-populate-prefix/src/imgui_color_text_edit-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
