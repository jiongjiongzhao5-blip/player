# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "Debug")
  file(REMOVE_RECURSE
  "CMakeFiles\\test_av_utils_autogen.dir\\AutogenUsed.txt"
  "CMakeFiles\\test_av_utils_autogen.dir\\ParseCache.txt"
  "CMakeFiles\\voice_player_learn_autogen.dir\\AutogenUsed.txt"
  "CMakeFiles\\voice_player_learn_autogen.dir\\ParseCache.txt"
  "test_av_utils_autogen"
  "voice_player_learn_autogen"
  )
endif()
