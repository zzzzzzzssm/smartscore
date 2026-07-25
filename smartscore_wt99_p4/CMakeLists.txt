cmake_minimum_required(VERSION 3.16)

# Build only the stage-one network firmware and its dependency graph.  Other
# placeholder components remain in the repository but are intentionally not
# part of this firmware image.
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(smartscore_wt99_p4)
