#define STRINGIZE2(s) #s
#define STRINGIZE(s) STRINGIZE2(s)

#define VERSION_MAJOR               0
#define VERSION_MINOR               8
#define VERSION_REVISION            0
#define VERSION_BUILD               724
#define VERSION_PREFIX              "Dev-"

#define GIT_REVISION                "795ab86df940634381610378293250ccf96de04e"
#define GIT_REVISION_SHORT          "795ab86"
#define GIT_DIRTY                   "Dirty"
#define GIT_VERSION                 "795ab86-Dirty"

#define VER_FILE_DESCRIPTION_STR    "NXEmu-Cpu"
#define VER_FILE_VERSION            VERSION_MAJOR, VERSION_MINOR, VERSION_REVISION, VERSION_BUILD
#define VER_FILE_VERSION_STR        VERSION_PREFIX STRINGIZE(VERSION_MAJOR)        \
                                    "." STRINGIZE(VERSION_MINOR)    \
                                    "." STRINGIZE(VERSION_REVISION) \
                                    "." STRINGIZE(VERSION_BUILD)    \
                                    "-" GIT_VERSION

#define VER_PRODUCTNAME_STR         "NXEmu-Cpu"
#define VER_PRODUCT_VERSION         VER_FILE_VERSION
#define VER_PRODUCT_VERSION_STR     VER_FILE_VERSION_STR
#define VER_ORIGINAL_FILENAME_STR   VER_PRODUCTNAME_STR ".dll"
#define VER_INTERNAL_NAME_STR       VER_PRODUCTNAME_STR
#define VERSION_BUILD_YEAR          2026
#define VER_COPYRIGHT_STR           "Copyright (C) " STRINGIZE(VERSION_BUILD_YEAR)

#ifdef _DEBUG
#define VER_VER_DEBUG             VS_FF_DEBUG
#else
#define VER_VER_DEBUG             0
#endif

#define VER_FILEOS                  VOS_NT_WINDOWS32
#define VER_FILEFLAGS               VER_VER_DEBUG
#define VER_FILETYPE                VFT_DLL

