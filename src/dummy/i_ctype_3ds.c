// DR. ROBOTNIK'S RING RACERS — 3DS ctype fix
//
// newlib's toupper/tolower use __ctype_ptr__, which points to locale tables
// in low memory (0x000000A0-0x000000C8). Azahar does not map that region ->
// unmapped Read32 in an infinite loop when entering the game loop.
//
// This file provides strong toupper/tolower symbols that replace
// newlib's implementation for the WHOLE binary (C and C++) without touching
// any header or breaking libstdc++'s locale_facets.h.
//
// Requires in CMakeLists.txt (SRB2SDL2 target, __3DS__ block):
//   target_compile_options(SRB2SDL2 PRIVATE
//       -fno-builtin-toupper -fno-builtin-tolower)

#ifdef __3DS__

// No ctype.h includes - avoid circular redeclaration
int toupper(int c);
int tolower(int c);

int toupper(int c)
{
    return (c >= 'a' && c <= 'z') ? (c - 32) : c;
}

int tolower(int c)
{
    return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
}

#endif // __3DS__
