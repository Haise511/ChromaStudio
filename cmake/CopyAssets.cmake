# Copies assets/textures and assets/materials (when present) next to the executable.
# cmake -DSRC=<source assets> -DDST=<destination assets> -P CopyAssets.cmake
foreach(sub textures materials)
    if(IS_DIRECTORY "${SRC}/${sub}")
        file(COPY "${SRC}/${sub}/" DESTINATION "${DST}/${sub}")
    endif()
endforeach()
