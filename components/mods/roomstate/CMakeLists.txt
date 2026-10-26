# SPDX-License-Identifier: GPL-3.0-or-later
# roomstate (docs/mods/roomstate.md): the store and the replay, no game
# types; roomstate_game.hpp, the hooks on them, is the game's to include.
add_library(mods_roomstate STATIC roomstate.cpp)
target_include_directories(mods_roomstate PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mods_roomstate PUBLIC log rules)
target_compile_features(mods_roomstate PUBLIC cxx_std_${CMAKE_CXX_STANDARD})
