# On the 3DS only the portable dependencies are wired up.
# Left out on purpose:
#   curl, cpptrace                     -> guarded per platform further down
#   discord-rpc, libwebm               -> Discord and WebM features are switched off through cmake
#   vma, volk, vulkan-headers          -> Vulkan, not applicable to the 3DS
#   tracy                              -> replaced by the no-op headers in tracy_stub/
if(CMAKE_SYSTEM_NAME STREQUAL "Generic" OR DEFINED DEVKITPRO)
	add_subdirectory(tcbrindle_span)
	add_subdirectory(fmt)
	add_subdirectory(glm)
	add_subdirectory(stb)
	add_subdirectory(xmp-lite)
	add_subdirectory(glad)
	add_subdirectory(imgui)
	add_subdirectory(renamenoise)
else()
	add_subdirectory(glm)
	if(SRB2_CONFIG_ENABLE_DISCORDRPC)
		add_subdirectory(discord-rpc)
	endif()
	add_subdirectory(xmp-lite)
	add_subdirectory(imgui)

	add_subdirectory(nlohmann-json)

	add_subdirectory(tcbrindle_span)
	add_subdirectory(stb)
	add_subdirectory(glad)
	add_subdirectory(tracy)

	add_subdirectory(libwebm)

	add_subdirectory(fmt)

	add_subdirectory(vulkan-headers)
	add_subdirectory(volk)
	add_subdirectory(vma)

	add_subdirectory(renamenoise)
endif()
