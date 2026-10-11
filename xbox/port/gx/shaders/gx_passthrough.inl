// gx_nv2a passthrough vertex program (CT-15). Derived from Starship xbox/vs_passthrough.vp.
// The CPU has already applied the GX position/projection matrices, texgen and lighting; this program
// only maps clip space to NV2A screen space and forwards the interpolants.
// Inputs : v[0]=POSITION (clip x,y,z,w)  v[3]=DIFFUSE (COLOR0)  v[4]=SPECULAR (COLOR1)
//          v[8..11]=TEXCOORD0..3 (s,t,r,q) -> hardware attribute slots 9..12
// Consts : c[0]..c[3] = viewport matrix rows (loaded at NV097_SET_TRANSFORM_CONSTANT_LOAD 96)
// Outputs: o[HPOS] = (screen xyz / w, w), o[COL0], o[COL1], o[TEX0..3]
0x00000000, 0x0020121b, 0x0836106c, 0x2070f848,
0x00000000, 0x0020141b, 0x0836106c, 0x2070f850,
0x00000000, 0x0020161b, 0x0836106c, 0x2070f858,
0x00000000, 0x0020181b, 0x0836106c, 0x2070f860,
0x00000000, 0x0020061b, 0x0836106c, 0x2070f818,
0x00000000, 0x0020081b, 0x0836106c, 0x2070f820,
0x00000000, 0x004c2055, 0x0836186c, 0x2f0007f8,
0x00000000, 0x008c0000, 0x0836186c, 0x1f0007f8,
0x00000000, 0x008c40aa, 0x0836186c, 0x1f0007f8,
0x00000000, 0x008c60ff, 0x0836186c, 0x1f0007f8,
0x00000000, 0x0400001b, 0x08361300, 0x101807f8,
0x00000000, 0x00400018, 0x0400286c, 0x2e0007f8,
0x00000000, 0x0020001b, 0x0436106c, 0x2070e800,
0x00000000, 0x002000ff, 0x0436106c, 0x20701801,
