target_sources(SRB2SDL2 PRIVATE
        i_net.c
        i_system.c
        i_main.c
        i_video.c
        i_sound.c
        i_3ds_stubs.c
        i_threads_stub.c
        i_3ds_rhi_stub.cpp
        i_video_3ds.c
        bottom_3ds.c
        twodee_soft_3ds.cpp
        gpu_present_3ds.c
        crashtrap_3ds.cpp
        memtrack_3ds.cpp
        fopen_count_3ds.c
        pre_3ds.c
        prof_3ds.c
)
# out-of-zone memory tracker (memtrack_3ds.cpp)
target_link_options(SRB2SDL2 PRIVATE "-Wl,--wrap=malloc,--wrap=free,--wrap=realloc,--wrap=calloc,--wrap=fopen,--wrap=fclose,--wrap=fread,--wrap=linearAlloc,--wrap=linearMemAlign")

set(SRB2_SDL2_AVAILABLE YES PARENT_SCOPE)
