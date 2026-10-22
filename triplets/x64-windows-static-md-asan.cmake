# vcpkg overlay triplet for the AddressSanitizer presets (msvc-asan, clangcl-asan).
#
# Same as the built-in x64-windows-static-md (static libraries, dynamic CRT),
# plus _ANNOTATE_STL. Background: under ASan, Microsoft's STL annotates
# std::string, std::vector and std::optional so ASan can report reads past
# size() but within capacity() ("container overflow"). Every object file
# records whether it uses those annotations, and the linker refuses to mix
# annotated and unannotated objects (LNK2038 'annotate_vector' etc.), because
# a half-annotated container produces false reports.
#
# _ANNOTATE_STL compiles the annotation code into these libraries without
# needing ASan for them; it switches on at run time only when the final
# program links the ASan runtime. Such libraries record no mismatch marker, so
# they link into both ASan and non-ASan programs, and ForgeDB's own code keeps
# full container-overflow detection. See the comment block at the top of the
# STL's <__msvc_sanitizer_annotate_container.hpp>.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_C_FLAGS "")
set(VCPKG_CXX_FLAGS "/D_ANNOTATE_STL")
