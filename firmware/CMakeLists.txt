cmake_minimum_required(VERSION 3.16)

# Must be set before project.cmake so the shared C library is a component.
set(EXTRA_COMPONENT_DIRS "../common")

# Stock ESP-IDF forces exFAT off. The volume-label option is also an unset
# Kconfig name, which fails to compile once the exFAT directory reader uses it
# as a C expression. Guard both so the field card (exFAT) can mount.
set(_ffconf "$ENV{IDF_PATH}/components/fatfs/src/ffconf.h")
if(EXISTS "${_ffconf}")
    file(READ "${_ffconf}" _ff_text)
    set(_ff_dirty 0)
    string(FIND "${_ff_text}" "#ifndef FF_FS_EXFAT" _ff_guard)
    if(_ff_guard EQUAL -1)
        string(REGEX REPLACE "#define[ \t]+FF_FS_EXFAT[ \t]+0"
            "#ifndef FF_FS_EXFAT\n#define FF_FS_EXFAT 0\n#endif" _ff_text "${_ff_text}")
        set(_ff_dirty 1)
    endif()
    string(REGEX MATCH "#define[ \t]+FF_USE_LABEL[ \t]+CONFIG_FATFS_USE_LABEL" _ff_label "${_ff_text}")
    if(_ff_label)
        string(REGEX REPLACE "#define[ \t]+FF_USE_LABEL[ \t]+CONFIG_FATFS_USE_LABEL"
            "#ifdef CONFIG_FATFS_USE_LABEL\n#define FF_USE_LABEL 1\n#else\n#define FF_USE_LABEL 0\n#endif"
            _ff_text "${_ff_text}")
        set(_ff_dirty 1)
    endif()
    if(_ff_dirty)
        file(WRITE "${_ffconf}" "${_ff_text}")
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cango)
